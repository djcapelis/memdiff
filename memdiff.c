/***********************************************************************
 * memdiff.c                                                           *
 *                                                                     *
 * The main program logic of memdiff, all in a jumble.                 *
 *                                                                     *
 **********************************************************************/

/* Includes */
#include<stdio.h>
#include<stdlib.h>
#include<unistd.h>
#include<stdbool.h>
#include<errno.h>
#include<string.h>
#include<limits.h>
#include<fcntl.h>

/* Includes from /sys */
#include<sys/stat.h>
#include<sys/mman.h>

/* Local project includes */
#include "djc_defines.h"

/* memdiff defines */
#define NAMELEN 256       /* Length of max file name */ 

/* Functions */
void print_usage();
void err_msg(char * msg);

/* I love the smell of global variables at 5 in the morning. */
/* Options */
bool OPT_S = false;
bool OPT_F = false;
bool OPT_P = false;
bool OPT_R = false;
bool OPT_B = false;
bool OPT_K = false;
bool OPT_Q = false;
bool OPT_D = false;

/* Does what it says on the tin */
void print_usage()
{
    fprintf(stderr, "Usage: memdiff -s <startsnap> -f <finalsnap> -p <pid> [options] [snapshot path]\n");
    fprintf(stderr, "\t-h Print usage\n");
    fprintf(stderr, "\t-s <num> Start at snapshot <num>\n");
    fprintf(stderr, "\t-f <num> Finish at snapshot <num>\n");
    fprintf(stderr, "\t-p <pid> Look for snapshots from pid <pid>\n");
    fprintf(stderr, "\t-r <num> Only examine region <num>\n");
    fprintf(stderr, "\t-b <size> Take differences in blocks <size> bytes\n");
    fprintf(stderr, "\t-k <size> Take differences in blocks <size> kilobytes\n");
    fprintf(stderr, "\t-q Be quiet, do not output anything but errors\n");
    fprintf(stderr, "\t-d <dir> Specify destination directory for diffs\n");
}

/* Entrypoint, argument parsing and core memory dumping functionality */
int main(int argc, char * argv[])
{
    /* Variable declarations */
    bool exit_error = false;    /* Used for error/exit handling */
    int startsnap = 1;          /* Current snapshot number */
    int termsnap;               /* Snapshot number we should end on, specified by option -f */
    int cursnap;                /* Current snapshot */
    int region;                 /* Region number */
    int curregion;              /* Current region */
    int destdirlen;             /* Keeps track of the length of the destination directory */
    int srcdirlen;              /* Keeps track of the length of the source directory */
    int pid;                    /* pid to snapshot */
    int blocksize = 4096;       /* diff blocksize */

    /* Things that need to be in scope to get cleaned up by error/exit handlers */
    char * destdir = NULL;      /* The destination directory, as specified by option -d */
    char * srcdir = NULL;       /* The source directory, as specified by the snapshot path */

    /* Filenames */
    char * src0 = NULL;
    char * src1 = NULL;
    char * dest = NULL;

    /* Filename rewrite pointers */
    char * src0rw = NULL;
    char * src1rw = NULL;
    char * destrw = NULL;

    /* File descriptors */
    int src0fd = 0;
    int src1fd = 0;
    int destfd = 0;

    /* Higher level file interfaces */
    off_t src0size = 0;
    off_t src1size = 0;
    char * map0 = NULL;         /* For mmap() on src0*/
    char * map1 = NULL;         /* For mmap() on src1 */
    FILE * destfile = NULL;     /* For the buffered streaming C standard lib file I/O interface for output files */

    /* Argument parsing */
    char opt;                   /* Opt for getopt() */
    char * strerr = NULL;       /* As per getopt() */
    long arg;                   /* Store argument input */
    struct stat statchk;        /* A stat struct to hold data from stat() in checks */
    int chk;                    /* A variable to hold return values for error checks */
    while((opt = getopt(argc, argv, "+hs:f:p:r:b:k:d:q")) != -1)
    {
        switch(opt)
        {
            /* Directory to put snapshot diffs in */
            case 'd':
                /* TODO: Unicode safe? */
                if(OPT_D) /* ... if argument already specified */
                    err_msg("Two or more -d arguments, please specify only one destination path\n");
                OPT_D = true;
                destdir = optarg;
                destdirlen = strlen(optarg);
                chk = stat(destdir, &statchk);
                if(chk == -1 && errno == ENOENT)
                    err_msg("Invalid path specified by -d option\n");
                else if(chk == -1)
                {
                    perror("Error parsing -d argument:"); /* Undefined error, handle using perror() */
                    exit(EXIT_FAILURE);
                }
                if(!S_ISDIR(statchk.st_mode))
                    err_msg("Path specified by -d is not a directory\n");
                optarg = NULL;
                break;
            /* Specify snapshot pid to diff */
            case 'p':
                OPT_P = true;
                arg = strtol(optarg, &strerr, 10);
                if(arg > INT_MAX || arg < 0 || strerr[0] != 0)
                    err_msg("Unable to parse -p argument correctly, should be a pid\n");
                pid = (pid_t) arg;
                optarg = NULL;
                break;
            /* Starting snapshot */
            case 's':
                OPT_S = true;
                arg = strtol(optarg, &strerr, 10);
                if(arg > INT_MAX || arg < 0 || strerr[0] != 0)
                    err_msg("Unable to parse -s argument correctly, should be starting snapshot number\n");
                startsnap = (int) arg;
                optarg = NULL;
                break;
            /* Final snapshot */
            case 'f':
                OPT_F = true;
                arg = strtol(optarg, &strerr, 10);
                if(arg > INT_MAX || arg < 0 || strerr[0] != 0)
                    err_msg("Unable to parse -f argument correctly, should be final snapshot number\n");
                termsnap = (int) arg;
                optarg = NULL;
                break;
            /* Region */
            case 'r':
                OPT_R = true;
                arg = strtol(optarg, &strerr, 10);
                if(arg > INT_MAX || arg < 0 || strerr[0] != 0)
                    err_msg("Unable to parse -r argument correctly, should be region number\n");
                region = (int) arg;
                optarg = NULL;
                break;
            /* Blocksize in bytes */
            case 'b':
                OPT_B = true;
                arg = strtol(optarg, &strerr, 10);
                if(arg > INT_MAX || arg < 0 || strerr[0] != 0)
                    err_msg("Unable to parse -b argument correctly, should be number of bytes\n");
                if((((arg - 1) & arg) != 0) || arg <= 0) /* If blocksize is not a positive, non-zero power of two */
                    err_msg("Unable to parse -b argument correctly.  Must be power of two.\n");
                blocksize = arg;
                optarg = NULL;
                break;
            /* Blocksize in kilobytes */
            case 'k':
                OPT_K = true;
                arg = strtol(optarg, &strerr, 10);
                if((arg * 1024) > INT_MAX || arg < 0 || strerr[0] != 0)
                    err_msg("Unable to parse -k argument correctly, should be number of kilobytes\n");
                if((((arg - 1) & arg) != 0) || (arg * 1024) <= 0) /* If blocksize is not a positive, non-zero power of two */
                    err_msg("Unable to parse -k argument correctly.  Must be power of two.\n");
                blocksize = arg * 1024;
                optarg = NULL;
                break;
            /* Set a flag to lessen the omit the routine output messages */
            case 'q':
                OPT_Q = true;
                break;
            /* Print usage and exit */
            case 'h':
            default:
                print_usage();
                return 0;
        }
    }
    /* Option validity checks */
    if(!OPT_S || !OPT_F || !OPT_P)
        err_msg("Options -s, -f and -p are all required in this release\n");
    if(startsnap >= termsnap)
        err_msg("The starting snapshot is not before the final snapshot.\n");
    if(argc <= optind) /* If there's no additional arguments... */
    {
        if(!OPT_Q)
            printf("%s\n", "No path to snapshots, searching current directory.");
        srcdirlen = 2;
        srcdir = calloc(1, 2);
        srcdir[0] = '.';
        srcdir[1] = '\0';
    }
    else
    {
        srcdirlen = strlen(argv[optind]);
        srcdir = argv[optind];
        chk = stat(srcdir, &statchk);
        if(chk == -1 && errno == ENOENT)
            err_msg("Invalid snapshot path\n");
        else if(chk == -1)
        {
            perror("Error parsing snapshot path:"); /* Undefined error, handle using perror() */
            exit(EXIT_FAILURE);
        }
        if(!S_ISDIR(statchk.st_mode))
            err_msg("Snapshot path is not a directory\n");
    }
    if(!OPT_D) /* If no destdir specifed, use source directory */
    {
        destdir = srcdir;
        destdirlen = srcdirlen;
    }

    /* Allocate memory for filenames and initialize related variables */
    src0 = calloc(1, srcdirlen + NAMELEN);
    src1 = calloc(1, srcdirlen + NAMELEN);
    dest = calloc(1, destdirlen + NAMELEN);
    strncpy(src0, srcdir, srcdirlen);
    strncpy(src1, srcdir, srcdirlen);
    strncpy(dest, destdir, destdirlen);
    src0rw = src0 + srcdirlen - 1;
    src1rw = src1 + srcdirlen - 1;
    destrw = dest + destdirlen - 1;

    /* Check for valid beginning and end snapshots */
    if(OPT_R)
    {
        snprintf(src0rw, NAMELEN, "%s%d%s%d%s%d", "/pid", pid, "_snap", startsnap, "_seg", region);
        snprintf(src1rw, NAMELEN, "%s%d%s%d%s%d", "/pid", pid, "_snap", termsnap, "_seg", region);
    }
    else
    {
        snprintf(src0rw, NAMELEN, "%s%d%s%d%s%d", "/pid", pid, "_snap", startsnap, "_seg", 0);
        snprintf(src1rw, NAMELEN, "%s%d%s%d%s%d", "/pid", pid, "_snap", termsnap, "_seg", 0);
    }

    /* Check for first snapshot */
    chk = stat(src0, &statchk);
    if(chk == -1 && errno==ENOENT)
    {
        fprintf(stderr, "Can't find first snapshot %s\n", src0);
        exit(EXIT_FAILURE);
    }
    else if(chk == -1)
    {
        perror("Error accessing snapshot:"); /* Undefined error */
        exit(EXIT_FAILURE);
    }

    /* Check for last snapshot */
    chk = stat(src1, &statchk);
    if(chk == -1 && errno==ENOENT)
    {
        fprintf(stderr, "Can't find last snapshot %s\n", src1);
        exit(EXIT_FAILURE);
    }
    else if(chk == -1)
    {
        perror("Error accessing snapshot:"); /* Undefined error */
        exit(EXIT_FAILURE);
    }

    /* We don't check for the middle ones because we assume they're probably there, if not, we'll error later */

    /* Begin main routine */
    for(cursnap = startsnap; cursnap < termsnap; cursnap++)
    {
        if(!OPT_Q)
            printf("Diffing snapshot %d and %d\n", cursnap, cursnap + 1);
        curregion = 0;
        while(1) /* Iterate through all regions */
        {
            if(OPT_R)
                curregion = region;

            /* Initialize filenames */
            if(OPT_R)
            {
                snprintf(src0rw, NAMELEN, "%s%d%s%d%s%d", "/pid", pid, "_snap", cursnap, "_seg", region);
                snprintf(src1rw, NAMELEN, "%s%d%s%d%s%d", "/pid", pid, "_snap", cursnap + 1, "_seg", region);
                snprintf(destrw, NAMELEN, "%s%d%s%d%s%d%s%d", "/diff:pid", pid, "_snap", cursnap, "_snap", cursnap + 1 , "_seg", region);
            }
            else
            {
                snprintf(src0rw, NAMELEN, "%s%d%s%d%s%d", "/pid", pid, "_snap", cursnap, "_seg", curregion);
                snprintf(src1rw, NAMELEN, "%s%d%s%d%s%d", "/pid", pid, "_snap", cursnap + 1, "_seg", curregion);
                snprintf(destrw, NAMELEN, "%s%d%s%d%s%d%s%d", "/diff:pid", pid, "_snap", cursnap, "_snap", cursnap + 1 , "_seg", curregion);
            }

            /* Open and check for errors */
            src0fd = open(src0, O_RDONLY);
            if(src0fd == -1 && errno == ENOENT)
                break; /* region doesn't exist, go to next snap */
            else if(src0fd == -1)
                cust_error(src0);

            src1fd = open(src1, O_RDONLY);
            if(src1fd == -1 && errno != ENOENT) /* If the file doesn't exist, we'll handle that later */
                cust_error(src1);

            destfd = open(dest, O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
            if(destfd == -1 && errno == EEXIST)
            {
                fprintf(stderr, "File %s already exists, aborting.\n", dest);
                exit_error = true;
                goto cleanup_and_term;
            }
            else if(destfd == -1)
                cust_error(dest);

            /* Higher layer bullshit */
            err_chk(stat(src0, &statchk) == -1);
            src0size = statchk.st_size;
            if(src1fd != -1)
            {
                err_chk(stat(src1, &statchk) == -1);
                src1size = statchk.st_size;
            }
            else
                src1size = 0;

            /* Memory maps for src0 and src1 */
            map0 = mmap(NULL, src0size, PROT_READ, MAP_PRIVATE, src0fd, 0);
            err_chk(map0 == MAP_FAILED);
            if(src1fd != -1)
            {
                map1 = mmap(NULL, src1size, PROT_READ, MAP_PRIVATE, src1fd, 0);
                err_chk(map1 == MAP_FAILED);
            }

            /* Stream interface for destfile */
            destfile = fdopen(destfd, "w");
            if(destfile == NULL)
                cust_error(dest);

            /* Differencing the memory */
            int curblock = 0;
            while(curblock * blocksize < src0size)
            {
                char write = '\0';
                for(int i = 0; i < 8; ++i)
                {
                    if(curblock * blocksize >= src0size) /* If we're done */
                    {
                        write = write << (8 - i);
                        ++curblock;
                        break;
                    }
                    if((curblock + 1) * blocksize > src0size)
                    {
                        if(memcmp(map0 + (curblock * blocksize), map1 + (curblock * blocksize), src0size % blocksize))
                            write = (write << 1) + 1;
                        else
                            write = write << 1;
                    }
                    else if((curblock + 1) * blocksize > src1size)
                        write = (write << 1) + 1;
                    else if(memcmp(map0 + (curblock * blocksize), map1 + (curblock * blocksize), blocksize))
                        write = (write << 1) + 1;
                    else
                        write = write << 1;
                    ++curblock;
                }
                err_chk(fwrite(&write, 1, 1, destfile) != 1)
            }


            /* Clean up */
            munmap(map0, src0size);
            map0 = NULL;
            munmap(map1, src1size);
            map1 = NULL;
            close(src0fd);
            src0fd = 0;
            close(src1fd);
            src1fd = 0;
            fclose(destfile);
            destfile = NULL;

            if(OPT_R)
                break;
            else
                curregion++;
        }
    }

    goto cleanup_and_term;

/* Error handler called by the err_chk macro */
err:
    perror("memdiff");
    exit_error = true;
    /* fallthrough */

/* We're done, exit cleanly */
cleanup_and_term:

/* Cleanup */
    if(OPT_D)
        free(destdir);
    if(src0)
        free(src0);
    if(src1)
        free(src1);
    if(dest && OPT_D)
        free(dest);
    if(destfile != NULL)
        fclose(destfile);
    if(map0 != NULL && map0 != MAP_FAILED)
        munmap(map0, src0size);
    if(map1 != NULL && map1 != MAP_FAILED)
        munmap(map1, src1size);
    if(src0fd > 0)
        close(src0fd);
    if(src0fd > 0)
        close(src1fd);

/* Exit */
    if(exit_error == true)
        exit(EXIT_FAILURE);
    exit(EXIT_SUCCESS);
}

/* Output an error message, print a usage message and bail */
void err_msg(char * msg)
{
    fprintf(stderr, "%s\n", msg);
    print_usage();
    exit(EXIT_FAILURE);
}
