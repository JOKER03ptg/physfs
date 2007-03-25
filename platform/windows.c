/*
 * Windows support routines for PhysicsFS.
 *
 * Please see the file LICENSE.txt in the source's root directory.
 *
 *  This file written by Ryan C. Gordon, and made sane by Gregory S. Read.
 */

#define __PHYSICSFS_INTERNAL__
#include "physfs_platforms.h"

#ifdef PHYSFS_PLATFORM_WINDOWS

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>

#include "physfs_internal.h"

#define LOWORDER_UINT64(pos) (PHYSFS_uint32) \
    (pos & 0x00000000FFFFFFFF)
#define HIGHORDER_UINT64(pos) (PHYSFS_uint32) \
    (((pos & 0xFFFFFFFF00000000) >> 32) & 0x00000000FFFFFFFF)

/*
 * Users without the platform SDK don't have this defined.  The original docs
 *  for SetFilePointer() just said to compare with 0xFFFFFFFF, so this should
 *  work as desired.
 */
#define PHYSFS_INVALID_SET_FILE_POINTER  0xFFFFFFFF

/* just in case... */
#define PHYSFS_INVALID_FILE_ATTRIBUTES   0xFFFFFFFF

#define UTF8_TO_UNICODE_STACK_MACRO(w_assignto, str) { \
    if (str == NULL) \
        w_assignto = NULL; \
    else { \
        const PHYSFS_uint64 len = (PHYSFS_uint64) ((strlen(str) * 4) + 1); \
        w_assignto = (char *) __PHYSFS_smallAlloc(len); \
        PHYSFS_uc2fromutf8(str, (PHYSFS_uint16 *) w_assignto, len); \
    } \
} \

typedef struct
{
    HANDLE handle;
    int readonly;
} win32file;

const char *__PHYSFS_platformDirSeparator = "\\";


/* pointers for APIs that may not exist on some Windows versions... */
static HANDLE libKernel32 = NULL;
static HANDLE libUserEnv = NULL;
static HANDLE libAdvApi32 = NULL;
static DWORD (WINAPI *pGetModuleFileNameA)(HMODULE, LPCH, DWORD);
static DWORD (WINAPI *pGetModuleFileNameW)(HMODULE, LPWCH, DWORD);
static BOOL (WINAPI *pGetUserProfileDirectoryW)(HANDLE, LPWSTR, LPDWORD);
static BOOL (WINAPI *pGetUserNameW)(LPWSTR, LPDWORD);
static DWORD (WINAPI *pGetFileAttributesW)(LPCWSTR);
static HANDLE (WINAPI *pFindFirstFileW)(LPCWSTR, LPWIN32_FIND_DATAW);
static BOOL (WINAPI *pFindNextFileW)(HANDLE, LPWIN32_FIND_DATAW);
static DWORD (WINAPI *pGetCurrentDirectoryW)(DWORD, LPWSTR);
static BOOL (WINAPI *pDeleteFileW)(LPCWSTR);
static BOOL (WINAPI *pRemoveDirectoryW)(LPCWSTR);
static BOOL (WINAPI *pCreateDirectoryW)(LPCWSTR, LPSECURITY_ATTRIBUTES);
static BOOL (WINAPI *pGetFileAttributesExA)
    (LPCSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
static BOOL (WINAPI *pGetFileAttributesExW)
    (LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
static DWORD (WINAPI *pFormatMessageW)
    (DWORD, LPCVOID, DWORD, DWORD, LPWSTR, DWORD, va_list *);
static DWORD (WINAPI *pSearchPathW)
    (LPCWSTR, LPCWSTR, LPCWSTR, DWORD, LPWSTR, LPWSTR *);
static HANDLE (WINAPI *pCreateFileW)
    (LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);

static char *userDir = NULL;


/* A blatant abuse of pointer casting... */
static void symLookup(HMODULE dll, void **addr, const char *sym)
{
    *addr = GetProcAddress(dll, sym);
} /* symLookup */


static int findApiSymbols(void)
{
    HMODULE dll = NULL;

    #define LOOKUP(x) { symLookup(dll, (void **) &p##x, #x); }

    dll = libUserEnv = LoadLibrary("userenv.dll");
    if (dll != NULL)
        LOOKUP(GetUserProfileDirectoryW);

    /* !!! FIXME: what do they call advapi32.dll on Win64? */
    dll = libAdvApi32 = LoadLibrary("advapi32.dll");
    if (dll != NULL)
        LOOKUP(GetUserNameW);

    /* !!! FIXME: what do they call kernel32.dll on Win64? */
    dll = libKernel32 = LoadLibrary("kernel32.dll");
    if (dll != NULL)
    {
        LOOKUP(GetModuleFileNameA);
        LOOKUP(GetModuleFileNameW);
        LOOKUP(FormatMessageW);
        LOOKUP(FindFirstFileW);
        LOOKUP(FindNextFileW);
        LOOKUP(GetFileAttributesW);
        LOOKUP(GetFileAttributesExA);
        LOOKUP(GetFileAttributesExW);
        LOOKUP(GetCurrentDirectoryW);
        LOOKUP(CreateDirectoryW);
        LOOKUP(RemoveDirectoryW);
        LOOKUP(CreateFileW);
        LOOKUP(DeleteFileW);
        LOOKUP(SearchPathW);
    } /* if */

    #undef LOOKUP

    return(1);
} /* findApiSymbols */


/*
 * Figure out what the last failing Win32 API call was, and
 *  generate a human-readable string for the error message.
 *
 * The return value is a static buffer that is overwritten with
 *  each call to this function.
 */
static const char *win32strerror(void)
{
    static TCHAR msgbuf[255];
    TCHAR *ptr = msgbuf;

    /* !!! FIXME: unicode version. */
    FormatMessage(
        FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        GetLastError(),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), /* Default language */
        msgbuf,
        sizeof (msgbuf) / sizeof (TCHAR),
        NULL 
    );

    /* chop off newlines. */
    for (ptr = msgbuf; *ptr; ptr++)
    {
        if ((*ptr == '\n') || (*ptr == '\r'))
        {
            *ptr = ' ';
            break;
        } /* if */
    } /* for */

    /* !!! FIXME: convert to UTF-8. */

    return((const char *) msgbuf);
} /* win32strerror */


static char *getExePath(void)
{
    DWORD buflen = 64;
    int success = 0;
    LPWSTR modpath = NULL;
    char *retval = NULL;

    while (1)
    {
        DWORD rc;
        void *ptr;

        if ( !(ptr = allocator.Realloc(modpath, buflen*sizeof(WCHAR))) )
        {
            allocator.Free(modpath);
            BAIL_MACRO(ERR_OUT_OF_MEMORY, NULL);
        } /* if */
        modpath = (LPWSTR) ptr;

        rc = pGetModuleFileNameW(NULL, modpath, buflen);
        if (rc == 0)
        {
            allocator.Free(modpath);
            BAIL_MACRO(win32strerror(), NULL);
        } /* if */

        if (rc < buflen)
        {
            buflen = rc;
            break;
        } /* if */

        buflen *= 2;
    } /* while */

    if (buflen > 0)  /* just in case... */
    {
        WCHAR *ptr = (modpath + buflen) - 1;
        while (ptr != modpath)
        {
            if (*ptr == '\\')
                break;
            ptr--;
        } /* while */

        if ((ptr == modpath) && (*ptr != '\\'))
            __PHYSFS_setError(ERR_GETMODFN_NO_DIR);
        else
        {
            *(ptr + 1) = '\0';  /* chop off filename. */
            retval = (char *) allocator.Malloc(buflen * 6);
            if (retval == NULL)
                __PHYSFS_setError(ERR_OUT_OF_MEMORY);
            else
                PHYSFS_utf8FromUcs2((const PHYSFS_uint16 *) modpath, retval, buflen * 6);
        } /* else */
    } /* else */
    allocator.Free(modpath);

    /* free up the bytes we didn't actually use. */
    if (retval != NULL)
    {
        void *ptr = allocator.Realloc(retval, strlen(retval) + 1);
        if (ptr != NULL)
            retval = (char *) ptr;
    } /* if */

    return(retval);   /* w00t. */
} /* getExePath */


/*
 * Try to make use of GetUserProfileDirectoryW(), which isn't available on
 *  some common variants of Win32. If we can't use this, we just punt and
 *  use the physfs base dir for the user dir, too.
 *
 * On success, module-scope variable (userDir) will have a pointer to
 *  a malloc()'d string of the user's profile dir, and a non-zero value is
 *  returned. If we can't determine the profile dir, (userDir) will
 *  be NULL, and zero is returned.
 */
static int determineUserDir(void)
{
    if (userDir != NULL)
        return(1);  /* already good to go. */

    /*
     * GetUserProfileDirectoryW() is only available on NT 4.0 and later.
     *  This means Win95/98/ME (and CE?) users have to do without, so for
     *  them, we'll default to the base directory when we can't get the
     *  function pointer. Since this is originally an NT API, we don't
	 *  offer a non-Unicode fallback.
     */
    if (pGetUserProfileDirectoryW != NULL)
    {
        HANDLE accessToken = NULL;       /* Security handle to process */
        HANDLE processHandle = GetCurrentProcess();
        if (OpenProcessToken(processHandle, TOKEN_QUERY, &accessToken))
        {
            DWORD psize = 0;
            WCHAR dummy = 0;
            LPWSTR wstr = NULL;
            BOOL rc = 0;

            /*
             * Should fail. Will write the size of the profile path in
             *  psize. Also note that the second parameter can't be
             *  NULL or the function fails.
             */	
    		rc = pGetUserProfileDirectoryW(accessToken, &dummy, &psize);
            assert(!rc);  /* !!! FIXME: handle this gracefully. */

            /* Allocate memory for the profile directory */
            wstr = (LPWSTR) __PHYSFS_smallAlloc(psize * sizeof (WCHAR));
            if (wstr != NULL)
            {
                if (pGetUserProfileDirectoryW(accessToken, wstr, &psize))
                {
                    const PHYSFS_uint64 buflen = psize * 6;
                    userDir = (char *) allocator.Malloc(buflen);
                    if (userDir != NULL)  
                        PHYSFS_utf8FromUcs2((const PHYSFS_uint16 *) wstr, userDir, buflen);
                    /* !!! FIXME: shrink allocation... */
                } /* if */
                __PHYSFS_smallFree(wstr);
            } /* else */
        } /* if */

        CloseHandle(accessToken);
    } /* if */

    if (userDir == NULL)  /* couldn't get profile for some reason. */
    {
        /* Might just be a non-NT system; resort to the basedir. */
        userDir = getExePath();
        BAIL_IF_MACRO(userDir == NULL, NULL, 0); /* STILL failed?! */
    } /* if */

    return(1);  /* We made it: hit the showers. */
} /* determineUserDir */


static BOOL mediaInDrive(const char *drive)
{
    UINT oldErrorMode;
    DWORD tmp;
    BOOL retval;

    /* Prevent windows warning message appearing when checking media size */
    oldErrorMode = SetErrorMode(SEM_FAILCRITICALERRORS);
    
    /* If this function succeeds, there's media in the drive */
    retval = GetVolumeInformation(drive, NULL, 0, NULL, NULL, &tmp, NULL, 0);

    /* Revert back to old windows error handler */
    SetErrorMode(oldErrorMode);

    return(retval);
} /* mediaInDrive */


void __PHYSFS_platformDetectAvailableCDs(PHYSFS_StringCallback cb, void *data)
{
    /* !!! FIXME: Can CD drives be non-drive letter paths? */
    /* !!! FIXME:  (so can they be Unicode paths?) */
    char drive_str[4] = "x:\\";
    char ch;
    for (ch = 'A'; ch <= 'Z'; ch++)
    {
        drive_str[0] = ch;
        if (GetDriveType(drive_str) == DRIVE_CDROM && mediaInDrive(drive_str))
            cb(data, drive_str);
    } /* for */
} /* __PHYSFS_platformDetectAvailableCDs */


char *__PHYSFS_platformCalcBaseDir(const char *argv0)
{
    if ((argv0 != NULL) && (strchr(argv0, '\\') != NULL))
        return(NULL); /* default behaviour can handle this. */

    return(getExePath());
} /* __PHYSFS_platformCalcBaseDir */


char *__PHYSFS_platformGetUserName(void)
{
    DWORD bufsize = 0;
    LPTSTR retval = NULL;

    /* !!! FIXME: unicode version. */
    if (GetUserName(NULL, &bufsize) == 0)  /* This SHOULD fail. */
    {
        retval = (LPTSTR) allocator.Malloc(bufsize);
        BAIL_IF_MACRO(retval == NULL, ERR_OUT_OF_MEMORY, NULL);
        /* !!! FIXME: unicode version. */
        if (GetUserName(retval, &bufsize) == 0)  /* ?! */
        {
            __PHYSFS_setError(win32strerror());
            allocator.Free(retval);
            retval = NULL;
        } /* if */
    } /* if */

    if (retval != NULL)
    {
        /* !!! FIXME: convert to UTF-8. */
    } /* if */

    return((char *) retval);
} /* __PHYSFS_platformGetUserName */


char *__PHYSFS_platformGetUserDir(void)
{
    char *retval = (char *) allocator.Malloc(strlen(userDir) + 1);
    BAIL_IF_MACRO(retval == NULL, ERR_OUT_OF_MEMORY, NULL);
    strcpy(retval, userDir); /* calculated at init time. */
    return(retval);
} /* __PHYSFS_platformGetUserDir */


PHYSFS_uint64 __PHYSFS_platformGetThreadID(void)
{
    return((PHYSFS_uint64) GetCurrentThreadId());
} /* __PHYSFS_platformGetThreadID */


int __PHYSFS_platformExists(const char *fname)
{
    BAIL_IF_MACRO
    (
        /* !!! FIXME: unicode version. */
        GetFileAttributes(fname) == PHYSFS_INVALID_FILE_ATTRIBUTES,
        win32strerror(), 0
    );
    return(1);
} /* __PHYSFS_platformExists */


int __PHYSFS_platformIsSymLink(const char *fname)
{
    /* !!! FIXME: Vista has symlinks. Recheck this. */
    return(0);  /* no symlinks on win32. */
} /* __PHYSFS_platformIsSymlink */


int __PHYSFS_platformIsDirectory(const char *fname)
{
    /* !!! FIXME: unicode version. */
    return((GetFileAttributes(fname) & FILE_ATTRIBUTE_DIRECTORY) != 0);
} /* __PHYSFS_platformIsDirectory */


char *__PHYSFS_platformCvtToDependent(const char *prepend,
                                      const char *dirName,
                                      const char *append)
{
    int len = ((prepend) ? strlen(prepend) : 0) +
              ((append) ? strlen(append) : 0) +
              strlen(dirName) + 1;
    char *retval = (char *) allocator.Malloc(len);
    char *p;

    BAIL_IF_MACRO(retval == NULL, ERR_OUT_OF_MEMORY, NULL);

    if (prepend)
        strcpy(retval, prepend);
    else
        retval[0] = '\0';

    strcat(retval, dirName);

    if (append)
        strcat(retval, append);

    for (p = strchr(retval, '/'); p != NULL; p = strchr(p + 1, '/'))
        *p = '\\';

    return(retval);
} /* __PHYSFS_platformCvtToDependent */


void __PHYSFS_platformEnumerateFiles(const char *dirname,
                                     int omitSymLinks,
                                     PHYSFS_EnumFilesCallback callback,
                                     const char *origdir,
                                     void *callbackdata)
{
    HANDLE dir;
    WIN32_FIND_DATA ent;
    size_t len = strlen(dirname);
    char *SearchPath;

    /* Allocate a new string for path, maybe '\\', "*", and NULL terminator */
    SearchPath = (char *) __PHYSFS_smallAlloc(len + 3);
    if (SearchPath == NULL)
        return;

    /* Copy current dirname */
    strcpy(SearchPath, dirname);

    /* if there's no '\\' at the end of the path, stick one in there. */
    if (SearchPath[len - 1] != '\\')
    {
        SearchPath[len++] = '\\';
        SearchPath[len] = '\0';
    } /* if */

    /* Append the "*" to the end of the string */
    strcat(SearchPath, "*");

    /* !!! FIXME: unicode version. */
    dir = FindFirstFile(SearchPath, &ent);
    __PHYSFS_smallFree(SearchPath);
    if (dir == INVALID_HANDLE_VALUE)
        return;

    do
    {
        /* !!! FIXME: unicode version. */
        if (strcmp(ent.cFileName, ".") == 0)
            continue;

        /* !!! FIXME: unicode version. */
        if (strcmp(ent.cFileName, "..") == 0)
            continue;

        callback(callbackdata, origdir, ent.cFileName);

        /* !!! FIXME: unicode version. */
    } while (FindNextFile(dir, &ent) != 0);

    FindClose(dir);
} /* __PHYSFS_platformEnumerateFiles */


char *__PHYSFS_platformCurrentDir(void)
{
    LPTSTR retval;
    DWORD buflen = 0;

    /* !!! FIXME: unicode version. */
    buflen = GetCurrentDirectory(buflen, NULL);
    retval = (LPTSTR) allocator.Malloc(sizeof (TCHAR) * (buflen + 2));
    BAIL_IF_MACRO(retval == NULL, ERR_OUT_OF_MEMORY, NULL);
    /* !!! FIXME: unicode version. */
    GetCurrentDirectory(buflen, retval);

    if (retval[buflen - 2] != '\\')
        strcat(retval, "\\");

    return((char *) retval);
} /* __PHYSFS_platformCurrentDir */


/* this could probably use a cleanup. */
char *__PHYSFS_platformRealPath(const char *path)
{
    /* this function should be UTF-8 clean. */
    char *retval = NULL;
    char *p = NULL;

    BAIL_IF_MACRO(path == NULL, ERR_INVALID_ARGUMENT, NULL);
    BAIL_IF_MACRO(*path == '\0', ERR_INVALID_ARGUMENT, NULL);

    retval = (char *) allocator.Malloc(MAX_PATH);
    BAIL_IF_MACRO(retval == NULL, ERR_OUT_OF_MEMORY, NULL);

        /*
         * If in \\server\path format, it's already an absolute path.
         *  We'll need to check for "." and ".." dirs, though, just in case.
         */
    if ((path[0] == '\\') && (path[1] == '\\'))
        strcpy(retval, path);

    else
    {
        char *currentDir = __PHYSFS_platformCurrentDir();
        if (currentDir == NULL)
        {
            allocator.Free(retval);
            BAIL_MACRO(ERR_OUT_OF_MEMORY, NULL);
        } /* if */

        if (path[1] == ':')   /* drive letter specified? */
        {
            /*
             * Apparently, "D:mypath" is the same as "D:\\mypath" if
             *  D: is not the current drive. However, if D: is the
             *  current drive, then "D:mypath" is a relative path. Ugh.
             */
            if (path[2] == '\\')  /* maybe an absolute path? */
                strcpy(retval, path);
            else  /* definitely an absolute path. */
            {
                if (path[0] == currentDir[0]) /* current drive; relative. */
                {
                    strcpy(retval, currentDir);
                    strcat(retval, path + 2);
                } /* if */

                else  /* not current drive; absolute. */
                {
                    retval[0] = path[0];
                    retval[1] = ':';
                    retval[2] = '\\';
                    strcpy(retval + 3, path + 2);
                } /* else */
            } /* else */
        } /* if */

        else  /* no drive letter specified. */
        {
            if (path[0] == '\\')  /* absolute path. */
            {
                retval[0] = currentDir[0];
                retval[1] = ':';
                strcpy(retval + 2, path);
            } /* if */
            else
            {
                strcpy(retval, currentDir);
                strcat(retval, path);
            } /* else */
        } /* else */

        allocator.Free(currentDir);
    } /* else */

    /* (whew.) Ok, now take out "." and ".." path entries... */

    p = retval;
    while ( (p = strstr(p, "\\.")) != NULL)
    {
        /* it's a "." entry that doesn't end the string. */
        if (p[2] == '\\')
            memmove(p + 1, p + 3, strlen(p + 3) + 1);

        /* it's a "." entry that ends the string. */
        else if (p[2] == '\0')
            p[0] = '\0';

        /* it's a ".." entry. */
        else if (p[2] == '.')
        {
            char *prevEntry = p - 1;
            while ((prevEntry != retval) && (*prevEntry != '\\'))
                prevEntry--;

            if (prevEntry == retval)  /* make it look like a "." entry. */
                memmove(p + 1, p + 2, strlen(p + 2) + 1);
            else
            {
                if (p[3] != '\0') /* doesn't end string. */
                    *prevEntry = '\0';
                else /* ends string. */
                    memmove(prevEntry + 1, p + 4, strlen(p + 4) + 1);

                p = prevEntry;
            } /* else */
        } /* else if */

        else
        {
            p++;  /* look past current char. */
        } /* else */
    } /* while */

    /* shrink the retval's memory block if possible... */
    p = (char *) allocator.Realloc(retval, strlen(retval) + 1);
    if (p != NULL)
        retval = p;

    return(retval);
} /* __PHYSFS_platformRealPath */


int __PHYSFS_platformMkDir(const char *path)
{
    /* !!! FIXME: unicode version. */
    DWORD rc = CreateDirectory(path, NULL);
    BAIL_IF_MACRO(rc == 0, win32strerror(), 0);
    return(1);
} /* __PHYSFS_platformMkDir */


/*
 * Get OS info and save the important parts.
 *
 * Returns non-zero if successful, otherwise it returns zero on failure.
 */
static int getOSInfo(void)
{
#if 0  /* we don't actually use this at the moment, but may in the future. */
    OSVERSIONINFO OSVersionInfo;     /* Information about the OS */
    OSVersionInfo.dwOSVersionInfoSize = sizeof(OSVersionInfo);
    BAIL_IF_MACRO(!GetVersionEx(&OSVersionInfo), win32strerror(), 0);

    /* Set to TRUE if we are runnign a WinNT based OS 4.0 or greater */
    runningNT = ((OSVersionInfo.dwPlatformId == VER_PLATFORM_WIN32_NT) &&
                 (OSVersionInfo.dwMajorVersion >= 4));
#endif

    return(1);
} /* getOSInfo */


int __PHYSFS_platformInit(void)
{
    BAIL_IF_MACRO(!findApiSymbols(), NULL, 0);
    BAIL_IF_MACRO(!getOSInfo(), NULL, 0);
    BAIL_IF_MACRO(!determineUserDir(), NULL, 0);
    return(1);  /* It's all good */
} /* __PHYSFS_platformInit */


int __PHYSFS_platformDeinit(void)
{
    HANDLE *libs[] = { &libKernel32, &libUserEnv, &libAdvApi32, NULL };
    int i;

    allocator.Free(userDir);
    userDir = NULL;

    for (i = 0; libs[i] != NULL; i++)
    {
        const HANDLE lib = *(libs[i]);
        if (lib)
            FreeLibrary(lib);
        *(libs[i]) = NULL;
    } /* for */

    return(1); /* It's all good */
} /* __PHYSFS_platformDeinit */


static void *doOpen(const char *fname, DWORD mode, DWORD creation, int rdonly)
{
    HANDLE fileHandle;
    win32file *retval;

    /* !!! FIXME: unicode version. */
    fileHandle = CreateFile(fname, mode, FILE_SHARE_READ, NULL,
                            creation, FILE_ATTRIBUTE_NORMAL, NULL);

    BAIL_IF_MACRO
    (
        fileHandle == INVALID_HANDLE_VALUE,
        win32strerror(), NULL
    );

    retval = (win32file *) allocator.Malloc(sizeof (win32file));
    if (retval == NULL)
    {
        CloseHandle(fileHandle);
        BAIL_MACRO(ERR_OUT_OF_MEMORY, NULL);
    } /* if */

    retval->readonly = rdonly;
    retval->handle = fileHandle;
    return(retval);
} /* doOpen */


void *__PHYSFS_platformOpenRead(const char *filename)
{
    return(doOpen(filename, GENERIC_READ, OPEN_EXISTING, 1));
} /* __PHYSFS_platformOpenRead */


void *__PHYSFS_platformOpenWrite(const char *filename)
{
    return(doOpen(filename, GENERIC_WRITE, CREATE_ALWAYS, 0));
} /* __PHYSFS_platformOpenWrite */


void *__PHYSFS_platformOpenAppend(const char *filename)
{
    void *retval = doOpen(filename, GENERIC_WRITE, OPEN_ALWAYS, 0);
    if (retval != NULL)
    {
        HANDLE h = ((win32file *) retval)->handle;
        DWORD rc = SetFilePointer(h, 0, NULL, FILE_END);
        if (rc == PHYSFS_INVALID_SET_FILE_POINTER)
        {
            const char *err = win32strerror();
            CloseHandle(h);
            allocator.Free(retval);
            BAIL_MACRO(err, NULL);
        } /* if */
    } /* if */

    return(retval);
} /* __PHYSFS_platformOpenAppend */


PHYSFS_sint64 __PHYSFS_platformRead(void *opaque, void *buffer,
                                    PHYSFS_uint32 size, PHYSFS_uint32 count)
{
    HANDLE Handle = ((win32file *) opaque)->handle;
    DWORD CountOfBytesRead;
    PHYSFS_sint64 retval;

    /* Read data from the file */
    /* !!! FIXME: uint32 might be a greater # than DWORD */
    if(!ReadFile(Handle, buffer, count * size, &CountOfBytesRead, NULL))
    {
        BAIL_MACRO(win32strerror(), -1);
    } /* if */
    else
    {
        /* Return the number of "objects" read. */
        /* !!! FIXME: What if not the right amount of bytes was read to make an object? */
        retval = CountOfBytesRead / size;
    } /* else */

    return(retval);
} /* __PHYSFS_platformRead */


PHYSFS_sint64 __PHYSFS_platformWrite(void *opaque, const void *buffer,
                                     PHYSFS_uint32 size, PHYSFS_uint32 count)
{
    HANDLE Handle = ((win32file *) opaque)->handle;
    DWORD CountOfBytesWritten;
    PHYSFS_sint64 retval;

    /* Read data from the file */
    /* !!! FIXME: uint32 might be a greater # than DWORD */
    if(!WriteFile(Handle, buffer, count * size, &CountOfBytesWritten, NULL))
    {
        BAIL_MACRO(win32strerror(), -1);
    } /* if */
    else
    {
        /* Return the number of "objects" read. */
        /* !!! FIXME: What if not the right number of bytes was written? */
        retval = CountOfBytesWritten / size;
    } /* else */

    return(retval);
} /* __PHYSFS_platformWrite */


int __PHYSFS_platformSeek(void *opaque, PHYSFS_uint64 pos)
{
    HANDLE Handle = ((win32file *) opaque)->handle;
    DWORD HighOrderPos;
    DWORD *pHighOrderPos;
    DWORD rc;

    /* Get the high order 32-bits of the position */
    HighOrderPos = HIGHORDER_UINT64(pos);

    /*
     * MSDN: "If you do not need the high-order 32 bits, this
     *         pointer must be set to NULL."
     */
    pHighOrderPos = (HighOrderPos) ? &HighOrderPos : NULL;

    /*
     * !!! FIXME: MSDN: "Windows Me/98/95:  If the pointer
     * !!! FIXME:  lpDistanceToMoveHigh is not NULL, then it must
     * !!! FIXME:  point to either 0, INVALID_SET_FILE_POINTER, or
     * !!! FIXME:  the sign extension of the value of lDistanceToMove.
     * !!! FIXME:  Any other value will be rejected."
     */

    /* Move pointer "pos" count from start of file */
    rc = SetFilePointer(Handle, LOWORDER_UINT64(pos),
                        pHighOrderPos, FILE_BEGIN);

    if ( (rc == PHYSFS_INVALID_SET_FILE_POINTER) &&
         (GetLastError() != NO_ERROR) )
    {
        BAIL_MACRO(win32strerror(), 0);
    } /* if */
    
    return(1);  /* No error occured */
} /* __PHYSFS_platformSeek */


PHYSFS_sint64 __PHYSFS_platformTell(void *opaque)
{
    HANDLE Handle = ((win32file *) opaque)->handle;
    DWORD HighPos = 0;
    DWORD LowPos;
    PHYSFS_sint64 retval;

    /* Get current position */
    LowPos = SetFilePointer(Handle, 0, &HighPos, FILE_CURRENT);
    if ( (LowPos == PHYSFS_INVALID_SET_FILE_POINTER) &&
         (GetLastError() != NO_ERROR) )
    {
        BAIL_MACRO(win32strerror(), 0);
    } /* if */
    else
    {
        /* Combine the high/low order to create the 64-bit position value */
        retval = (((PHYSFS_uint64) HighPos) << 32) | LowPos;
        assert(retval >= 0);
    } /* else */

    return(retval);
} /* __PHYSFS_platformTell */


PHYSFS_sint64 __PHYSFS_platformFileLength(void *opaque)
{
    HANDLE Handle = ((win32file *) opaque)->handle;
    DWORD SizeHigh;
    DWORD SizeLow;
    PHYSFS_sint64 retval;

    SizeLow = GetFileSize(Handle, &SizeHigh);
    if ( (SizeLow == PHYSFS_INVALID_SET_FILE_POINTER) &&
         (GetLastError() != NO_ERROR) )
    {
        BAIL_MACRO(win32strerror(), -1);
    } /* if */
    else
    {
        /* Combine the high/low order to create the 64-bit position value */
        retval = (((PHYSFS_uint64) SizeHigh) << 32) | SizeLow;
        assert(retval >= 0);
    } /* else */

    return(retval);
} /* __PHYSFS_platformFileLength */


int __PHYSFS_platformEOF(void *opaque)
{
    PHYSFS_sint64 FilePosition;
    int retval = 0;

    /* Get the current position in the file */
    if ((FilePosition = __PHYSFS_platformTell(opaque)) != 0)
    {
        /* Non-zero if EOF is equal to the file length */
        retval = FilePosition == __PHYSFS_platformFileLength(opaque);
    } /* if */

    return(retval);
} /* __PHYSFS_platformEOF */


int __PHYSFS_platformFlush(void *opaque)
{
    win32file *fh = ((win32file *) opaque);
    if (!fh->readonly)
        BAIL_IF_MACRO(!FlushFileBuffers(fh->handle), win32strerror(), 0);

    return(1);
} /* __PHYSFS_platformFlush */


int __PHYSFS_platformClose(void *opaque)
{
    HANDLE Handle = ((win32file *) opaque)->handle;
    BAIL_IF_MACRO(!CloseHandle(Handle), win32strerror(), 0);
    allocator.Free(opaque);
    return(1);
} /* __PHYSFS_platformClose */


int __PHYSFS_platformDelete(const char *path)
{
    /* If filename is a folder */
    if (GetFileAttributes(path) == FILE_ATTRIBUTE_DIRECTORY)
    {
        /* !!! FIXME: unicode version. */
        BAIL_IF_MACRO(!RemoveDirectory(path), win32strerror(), 0);
    } /* if */
    else
    {
        /* !!! FIXME: unicode version. */
        BAIL_IF_MACRO(!DeleteFile(path), win32strerror(), 0);
    } /* else */

    return(1);  /* if you got here, it worked. */
} /* __PHYSFS_platformDelete */


/*
 * !!! FIXME: why aren't we using Critical Sections instead of Mutexes?
 * !!! FIXME:  mutexes on Windows are for cross-process sync. CritSects are
 * !!! FIXME:  mutexes for threads in a single process and are faster.
 */
void *__PHYSFS_platformCreateMutex(void)
{
    return((void *) CreateMutex(NULL, FALSE, NULL));
} /* __PHYSFS_platformCreateMutex */


void __PHYSFS_platformDestroyMutex(void *mutex)
{
    CloseHandle((HANDLE) mutex);
} /* __PHYSFS_platformDestroyMutex */


int __PHYSFS_platformGrabMutex(void *mutex)
{
    return(WaitForSingleObject((HANDLE) mutex, INFINITE) != WAIT_FAILED);
} /* __PHYSFS_platformGrabMutex */


void __PHYSFS_platformReleaseMutex(void *mutex)
{
    ReleaseMutex((HANDLE) mutex);
} /* __PHYSFS_platformReleaseMutex */


static PHYSFS_sint64 FileTimeToPhysfsTime(const FILETIME *ft)
{
    SYSTEMTIME st_utc;
    SYSTEMTIME st_localtz;
    TIME_ZONE_INFORMATION tzi;
    DWORD tzid;
    PHYSFS_sint64 retval;
    struct tm tm;

    BAIL_IF_MACRO(!FileTimeToSystemTime(ft, &st_utc), win32strerror(), -1);
    tzid = GetTimeZoneInformation(&tzi);
    BAIL_IF_MACRO(tzid == TIME_ZONE_ID_INVALID, win32strerror(), -1);

        /* (This API is unsupported and fails on non-NT systems. */
    if (!SystemTimeToTzSpecificLocalTime(&tzi, &st_utc, &st_localtz))
    {
        /* do it by hand. Grumble... */
        ULARGE_INTEGER ui64;
        FILETIME new_ft;
        ui64.LowPart = ft->dwLowDateTime;
        ui64.HighPart = ft->dwHighDateTime;

        if (tzid == TIME_ZONE_ID_STANDARD)
            tzi.Bias += tzi.StandardBias;
        else if (tzid == TIME_ZONE_ID_DAYLIGHT)
            tzi.Bias += tzi.DaylightBias;

        /* convert from minutes to 100-nanosecond increments... */
        #if 0 /* For compilers that puke on 64-bit math. */
            /* goddamn this is inefficient... */
            while (tzi.Bias > 0)
            {
                DWORD tmp = ui64.LowPart - 60000000;
                if ((ui64.LowPart < tmp) && (tmp > 60000000))
                    ui64.HighPart--;
                ui64.LowPart = tmp;
                tzi.Bias--;
            } /* while */

            while (tzi.Bias < 0)
            {
                DWORD tmp = ui64.LowPart + 60000000;
                if ((ui64.LowPart > tmp) && (tmp < 60000000))
                    ui64.HighPart++;
                ui64.LowPart = tmp;
                tzi.Bias++;
            } /* while */
        #else
            ui64.QuadPart -= (((LONGLONG) tzi.Bias) * (600000000));
        #endif

        /* Move it back into a FILETIME structure... */
        new_ft.dwLowDateTime = ui64.LowPart;
        new_ft.dwHighDateTime = ui64.HighPart;

        /* Convert to something human-readable... */
        if (!FileTimeToSystemTime(&new_ft, &st_localtz))
            BAIL_MACRO(win32strerror(), -1);
    } /* if */

    /* Convert to a format that mktime() can grok... */
    tm.tm_sec = st_localtz.wSecond;
    tm.tm_min = st_localtz.wMinute;
    tm.tm_hour = st_localtz.wHour;
    tm.tm_mday = st_localtz.wDay;
    tm.tm_mon = st_localtz.wMonth - 1;
    tm.tm_year = st_localtz.wYear - 1900;
    tm.tm_wday = -1 /*st_localtz.wDayOfWeek*/;
    tm.tm_yday = -1;
    tm.tm_isdst = -1;

    /* Convert to a format PhysicsFS can grok... */
    retval = (PHYSFS_sint64) mktime(&tm);
    BAIL_IF_MACRO(retval == -1, strerror(errno), -1);
    return(retval);
} /* FileTimeToPhysfsTime */


PHYSFS_sint64 __PHYSFS_platformGetLastModTime(const char *fname)
{
    PHYSFS_sint64 retval = -1;
    WIN32_FILE_ATTRIBUTE_DATA attrData;
    memset(&attrData, '\0', sizeof (attrData));

    /* GetFileAttributesEx didn't show up until Win98 and NT4. */
    if (pGetFileAttributesExA != NULL)
    {
        /* !!! FIXME: unicode version. */
        if (pGetFileAttributesExA(fname, GetFileExInfoStandard, &attrData))
        {
            /* 0 return value indicates an error or not supported */
            if ( (attrData.ftLastWriteTime.dwHighDateTime != 0) ||
                 (attrData.ftLastWriteTime.dwLowDateTime != 0) )
            {
                retval = FileTimeToPhysfsTime(&attrData.ftLastWriteTime);
            } /* if */
        } /* if */
    } /* if */

    /* GetFileTime() has been in the Win32 API since the start. */
    if (retval == -1)  /* try a fallback... */
    {
        FILETIME ft;
        BOOL rc;
        const char *err;
        win32file *f = (win32file *) __PHYSFS_platformOpenRead(fname);
        BAIL_IF_MACRO(f == NULL, NULL, -1)
        rc = GetFileTime(f->handle, NULL, NULL, &ft);
        err = win32strerror();
        CloseHandle(f->handle);
        allocator.Free(f);
        BAIL_IF_MACRO(!rc, err, -1);
        retval = FileTimeToPhysfsTime(&ft);
    } /* if */

    return(retval);
} /* __PHYSFS_platformGetLastModTime */


/* !!! FIXME: Don't use C runtime for allocators? */
int __PHYSFS_platformSetDefaultAllocator(PHYSFS_Allocator *a)
{
    return(0);  /* just use malloc() and friends. */
} /* __PHYSFS_platformSetDefaultAllocator */

#endif  /* PHYSFS_PLATFORM_WINDOWS */

/* end of windows.c ... */


