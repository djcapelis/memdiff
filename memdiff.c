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

/* Includes from /sys */
#include<sys/stat.h>

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
    int destdirlen;             /* Keeps track of the length of the destination directory */
    int srcdirlen;              /* Keeps track of the length of the source directory */
    int pid;                    /* pid to snapshot */
    int region;                 /* region number */
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

    /* Argument parsing */
    char opt;
    char * strerr = NULL;
    long arg;
    struct stat dirstat;
    int chk;
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
                chk = stat(destdir, &dirstat);
                if(chk == -1 && errno == ENOENT)
                    err_msg("Invalid path specified by -d option\n");
                else if(chk == -1)
                {
                    perror("Error parsing -d argument:"); /* Undefined error, handle using perror() */
                    exit(EXIT_FAILURE);
                }
                if(!S_ISDIR(dirstat.st_mode))
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
    if(!OPT_D) /* Set default destdir to current directory if none is specified */
    {
        destdirlen = 2; /* extra room because it's, just shush */
        destdir = calloc(1, 2);
        destdir[0] = '.';
        destdir[1] = '\0';
    }
    if(argc <= optind)
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
        chk = stat(srcdir, &dirstat);
        if(chk == -1 && errno == ENOENT)
            err_msg("Invalid snapshot path\n");
        else if(chk == -1)
        {
            perror("Error parsing snapshot path:"); /* Undefined error, handle using perror() */
            exit(EXIT_FAILURE);
        }
        if(!S_ISDIR(dirstat.st_mode))
            err_msg("Snapshot path is not a directory\n");
    }

    /* Allocate memory for filenames and initialize them */
    src0 = calloc(1, srcdirlen + NAMELEN);
    src1 = calloc(1, srcdirlen + NAMELEN);
    dest = calloc(1, destdirlen + NAMELEN);
    strncpy(src0, srcdir, srcdirlen);
    strncpy(src1, srcdir, srcdirlen);
    strncpy(dest, destdir, destdirlen);
    src0rw = src0 + srcdirlen - 1;
    src1rw = src1 + srcdirlen - 1;
    destrw = dest + destdirlen - 1;
    snprintf(src1rw, NAMELEN, "%s%d%s%d%s%d", "/pid", pid, "_snap", startsnap, "_seg", 0);

    /* Begin main routine */
    for(int cursnap = startsnap; cursnap < termsnap; cursnap++)
    {
        if(!OPT_Q)
            printf("Diffing snapshot %d and %d\n", cursnap, cursnap + 1);
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
    if(!OPT_D)
        free(destdir);
    if(src0)
        free(src0);
    if(src1)
        free(src1);
    if(dest)
        free(dest);

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
