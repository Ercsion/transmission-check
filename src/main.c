/*
This file is part of transmission-check.

transmission-check is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

transmission-check is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with transmission-check.  If not, see <http://www.gnu.org/licenses/>.

Copyright 2016 Ysard
*/

#define _FILE_OFFSET_BITS 64 // Fix warning "stat: Value too large for defined data type"
#include <locale.h>
#include <signal.h>
#include <string.h> // strlen(), strstr(), strcmp()
#include <stdio.h> // fprintf(), printf()
#include <stdlib.h> // exit(), EXIT_FAILURE, EXIT_SUCCESS
#include <time.h> // ctime(), localtime()
#include <inttypes.h> // http://en.cppreference.com/w/cpp/types/integer - uint64_t on printf()
#include <sys/types.h>
#include <sys/stat.h> // stat()
#include <ftw.h> // ftw()
#include <regex.h> // regexec(), regerror(), regfree(), regcomp()
#include <libgen.h> // basename()
#include <errno.h>
#include <unistd.h>
// libtransmission
#include <libtransmission/transmission.h>
#include <libtransmission/variant.h>
#include <libtransmission/tr-getopt.h> // command line parser

#define MY_NAME "transmission-check"
#define LONG_VERSION_STRING "0.1"
#define PRINT_MEMORY_ERROR() fprintf(stderr, "ERROR: Insufficient memory\n\n");


// Global static variables
static uint64_t total_size = 0;
static int nb_repaired_inconsistencies = 0;

// Parameters
static bool make_changes = false;
static bool showVersion = false;
static bool verbose = false;
static const char * resume_file = NULL;
static const char * replace[2] = { NULL, NULL };

static tr_option options[] =
{
    { 'm', "make-changes", "Make changes on resume file", "m", 0, NULL },
    { 'r', "replace", "Search and replace a substring in the filepath", "r", 1, "<old> <new>" },
    { 'v', "verbose", "Display informations about resume file", "v", 0, NULL },
    { 'V', "version", "Show version number and exit", "V", 0, NULL },
    { 0, NULL, NULL, NULL, 0, NULL }
};


static const char * getUsage (void)
{
    return "Usage: " MY_NAME " [options] resume-file";
}


static int parseCommandLine (int argc, const char ** argv)
{
    /* Command line parser provided by transmission project
     */

    int c;
    const char * optarg;

    while ((c = tr_getopt (getUsage (), argc, argv, options, &optarg)))
    {
        switch (c)
        {
        case 'm':
            make_changes = true;
            break;

        case 'r':
            replace[0] = optarg;
            c = tr_getopt (getUsage (), argc, argv, options, &optarg);
            if (c != TR_OPT_UNK)
                return 1;
            replace[1] = optarg;
            break;

        case 'v':
            verbose = true;
            break;

        case 'V':
            showVersion = true;
            break;

        case TR_OPT_UNK:
            if (resume_file != NULL)
                return 1;
            resume_file = optarg;
            break;

        default:
            return 1;
        }
    }

    return 0;
}


int is_file_or_dir_exists(const char *path)
{
    /* Detect if the given file/directory exists.
     * Return 0 if does not exist or if the type is unknown/not supported.
     * Return 1,2 or 3 if exists and is a directory, symlink or regular file.
     */

    struct stat info;
    int err = 0;

    // On success, zero is returned. On error, -1 is returned, and errno is set appropriately.
    err = stat(path, &info);

    if(err == -1) {
        if(errno == ENOENT) {
            /* does not exist */
            return 0;
        } else {
            perror("stat");
            exit(EXIT_FAILURE);
        }
    }

    // directory : info.st_mode & S_IFDIR
    switch (info.st_mode & S_IFMT) {
    case S_IFDIR:  printf("Directory found... ");        return 1;
    case S_IFLNK:  printf("Symlink found... ");          return 2;
    case S_IFREG:  printf("Regular file found... ");     return 3;
    default:       printf("Unknown type?\n");            return 0;
    }

    return 0;
}


int sum_sizes(const char *fpath __attribute__((unused)), const struct stat *sb, int typeflag __attribute__((unused)))
{
    /* Callback for ftw() function.
     * Calculate the size of the given file and increment static 'total_size' variable.
     */

    total_size += sb->st_size;
    return 0;
}


void check_uploaded_files(char ** full_path)
{
    /* Test the existence of the given file/directory.
     * Calculate the size.
     */

    int err = 0;

    if (is_file_or_dir_exists(*full_path) > 0) {

        err = ftw(*full_path, &sum_sizes, 1);

        if (err == -1) {
            perror("ftw");
            free(full_path);
            exit(EXIT_FAILURE);
        }

    } else {
        fprintf(stderr, "ERROR: Uploaded file/directory '%s' not found !\n", *full_path);
        free(*full_path);
        exit(EXIT_FAILURE);
    }

    printf("Total bytes: %" PRIu64 "\n", total_size);
}


void get_uploaded_files_path(tr_variant * top, char ** full_path)
{
    /* Compute the full path of file/directory downloaded by the torrent.
     */

    char * tmp_ptr = NULL;
    size_t len;
    const char * str;

    // Concatenate paths dynamically.
    if ((tr_variantDictFindStr (top, TR_KEY_destination, &str, &len))
            && (str && *str))
    {
        //printf("TR_KEY_destination %s, %zu\n", str, len);

        *full_path = malloc((len + 1) * sizeof(**full_path));

        if (*full_path) {
            strcpy(*full_path, str);
            //printf("dir path: %s, %zu\n", *full_path, strlen(*full_path));

            if (tr_variantDictFindStr (top, TR_KEY_name, &str, &len))
            {
                //printf("TR_KEY_name %s, %zu\n", str, len);

                // Temp pointer
                // length of previous string + length of added string + length if '/' + '\0'
                tmp_ptr = realloc(*full_path, (strlen(*full_path) + len + 2) * sizeof(**full_path));

                if (tmp_ptr == NULL) {
                    // Free memory
                    free(*full_path);
                    exit(EXIT_FAILURE);
                } else {
                    *full_path = tmp_ptr;
                }

                strcat(*full_path, "/");
                strncat(*full_path, str, len);

                //printf("full path: %s\n", *full_path);
                return;

            } else {
                fprintf(stderr, "ERROR: Resume file: TR_KEY_name could not be read !\n");

                // On error, deallocate the memory
                free(*full_path);
                exit(EXIT_FAILURE);
            }
        } else {
            PRINT_MEMORY_ERROR()
            exit(EXIT_FAILURE);
        }

    } else {
        fprintf(stderr, "ERROR: Resume file: TR_KEY_destination could not be read !\n");
    }
}


void update_dates(tr_variant * top, char date_name[], const tr_quark date_type,
                  int64_t old_timestamp, time_t new_timestamp,
                  bool force_date_update, bool make_changes)
{
    /* Update the dates according to the given parameters:
     * old_timestamp is replaced by new_timestamp
     * if make_changes is true:
     * and date is erroneous
     * or force_date_update is true
     *
     * The date type (key in tr_variant dict is given by 'date_type';
     * the name of the manipulated date is given by the string 'date_name'.
     *
     * In case of replacement, new date is the last file modification date.
     */

    struct tm instant;

    instant = *localtime((time_t*)&old_timestamp);
    // printf("%s date: %s %" PRIu64 "\n", date_name, ctime(&old_timestamp), old_timestamp);

    // Change date if it is Erroneous or if force_date_update is set to true
    if (instant.tm_year + 1900 == 1970 || force_date_update) {

        if (make_changes) {
            tr_variantDictAddInt(top, date_type, new_timestamp);
            printf("REPAIR: Erroneous %s date: Updated to modification date: %s", date_name, ctime((time_t*)&new_timestamp));

            nb_repaired_inconsistencies++;
        } else {
            // Just inform that an erroneous date was encountered...
            printf("Erroneous %s date detected !\n", date_name);
        }
    }
}


void check_dates(tr_variant * top, char ** full_path, bool force_date_update, bool make_changes)
{
    /* Try to resolve date problems (incorrect/corrupted dates)
     * On error => update the field with last file modification date.
     */

    struct stat sb;
    int err = 0;
    int64_t  old_timestamp;

    err = stat(*full_path, &sb);

    if(err == -1) {
        perror("stat");
        free(*full_path);
        exit(EXIT_FAILURE);
    }

    /*
    printf("Last status change:       %s", ctime(&sb.st_ctime));
    printf("Last file access:         %s", ctime(&sb.st_atime));
    printf("Last file modification:   %s", ctime(&sb.st_mtime));
    */

    if (tr_variantDictFindInt (top, TR_KEY_added_date, &old_timestamp))
    {
        update_dates(top, "added", TR_KEY_added_date,
                     old_timestamp, sb.st_mtime,
                     force_date_update, make_changes);
    }

    if (tr_variantDictFindInt (top, TR_KEY_done_date, &old_timestamp))
    {
        update_dates(top, "done", TR_KEY_done_date,
                     old_timestamp, sb.st_mtime,
                     force_date_update, make_changes);
    }
}


void reset_peers(tr_variant * top)
{
    /* Reset peers in the resume file.
     */

    size_t len;
    const uint8_t * str_8;

    if (tr_variantDictFindRaw (top, TR_KEY_peers2, &str_8, &len))
    {
        // reinit peers
        tr_variantDictAddRaw (top, TR_KEY_peers2, NULL, 0);
    }

    if (tr_variantDictFindRaw (top, TR_KEY_peers2_6, &str_8, &len))
    {
        // reinit peers
        tr_variantDictAddRaw (top, TR_KEY_peers2_6, NULL, 0);
    }

    printf("REPAIR: Peers cleared.\n");
}


void check_correct_files_pointed(tr_variant * top, const char resume_filename[])
{
    /* Verify if file/directory of the torrent matches the resume filename.
     * If not, we try to infer the original name from the name of the resume filename.
     * In this case, nb_repaired_inconsistencies is incremented.
     * Note: If nb_repaired_inconsistencies is incremented here,
     * full_path variable must be updated with the new inferred file
     * (and we have to verify of the inferred file exists).
     * Note: Since corrupted resume files get their dates from the bad pointed file,
     * dates must also be updated, even if they are correct (above 1970 ...).
     */

    size_t len;
    int err = 0;
    regex_t preg;
    const char * actual_file;
    const char * regex_suffix = "\\.([[:lower:][:digit:]]{16})\\.resume$";


    // Get file downloaded
    tr_variantDictFindStr(top, TR_KEY_name, &actual_file, &len);
    //printf("%s VS %s\n", actual_file, resume_filename);

    // No pb in file names
    if(strstr(resume_filename, actual_file) != NULL) {
        //printf("File/directory name matches !\n");
        return;
    }


    printf("REPAIR: Resume file does not point to the correct file/directory !\n");
    printf("REPAIR: Trying to resolve inconsistencies...\n");


    err = regcomp (&preg, regex_suffix, REG_EXTENDED);

    if (err == 0) {

        int match;
        size_t nmatch = 0;
        regmatch_t *pmatch = NULL;

        nmatch = preg.re_nsub;
        pmatch = malloc (sizeof (*pmatch) * nmatch);

        // Now, we know that there are nmatch matches.
        if (pmatch) {

            match = regexec (&preg, resume_filename, nmatch, pmatch, 0);
            regfree (&preg);

            if (match == 0) {

                char * inferred_file = NULL;
                int start = pmatch[0].rm_so;
                //int end = pmatch[0].rm_eo;
                //size_t size = end - start;
                //char * suffix = NULL;
                /*
                suffix = malloc (sizeof (*suffix) * (size + 1));

                if (suffix != NULL) {
                    strncpy(suffix, &resume_filename[start], size);
                    suffix[size] = '\0';
                    printf("suffix %s\n", suffix);
                    free(suffix);
                }
                */

                //printf("suffix found at position %d\n", start);
                //printf("%s\n", resume_filename);

                // get [0;x] included, where x is the position of the first character of the suffix
                inferred_file = malloc (sizeof (*inferred_file) * (start + 1));

                if (inferred_file) {
                    strncpy(inferred_file, &resume_filename[0], start);
                    inferred_file[start] = '\0';

                    printf("REPAIR: Inferred file: %s\n", inferred_file);

                    // Update the resume file
                    tr_variantDictAddStr(top, TR_KEY_name, inferred_file);
                    nb_repaired_inconsistencies++;

                    // Deallocate memory
                    free(inferred_file);
                } else {
                    PRINT_MEMORY_ERROR()
                    exit(EXIT_FAILURE);
                }

            } else if (match == REG_NOMATCH) {
                fprintf(stderr, "ERROR: Resume file has an incorrect name !\n");
                exit(EXIT_FAILURE);

            } else {
                // Regex error handling

                char *text;
                size_t size;

                size = regerror(err, &preg, NULL, 0);
                text = malloc(sizeof (*text) * size);

                if (text) {

                    regerror(err, &preg, text, size);
                    fprintf(stderr, "ERROR: Regex: %s\n", text);

                    // Free memory
                    free(text);

                } else {
                    PRINT_MEMORY_ERROR()
                    exit(EXIT_FAILURE);
                }
            }
            // Free memory
            free(pmatch);
        } else {
            PRINT_MEMORY_ERROR()
            exit(EXIT_FAILURE);
        }
    }
}


void replace_dir(tr_variant * top, const char old[], const char new[])
{
    /* Replace old substring in path by the new string
     */

    size_t len;
    const char * str;
    char * start = NULL;
    char * new_path = NULL;


    // Concatenate paths dynamically.
    if ((tr_variantDictFindStr (top, TR_KEY_destination, &str, &len))
            && (str && *str))
    {

        start = strstr(str, old);
        /* printf("original dest: %s\n", str);
         * printf("addr: %d %c\n", start, *start);
         * printf("addr+1: %d, %c\n", start+1, *(start+1));
         * printf("original dest index 0: %d, %c\n", str, str[0]);
         * printf("suffix start addr: %d, %c\n", start+strlen(old), *(start+strlen(old)));
         * printf("suffix end addr: %d, %c\n", str + strlen(str), *(str + strlen(str)));
         * printf("alloc size %d\n", strlen(str) - strlen(old) + strlen(new) + 1);
         */

        if(start) {

            size_t prefix_length = start - str;
            char * suffix_start_addr = start + strlen(old);

            new_path = calloc((strlen(str) - strlen(old) + strlen(new) + 1), sizeof(*new_path));

            if (new_path) {
                // Add prefix
                strncpy(new_path, str, prefix_length);
                // Add new string
                strcat(new_path, new);
                // Add suffix
                strcat(new_path, suffix_start_addr);

                // Update the resume file
                tr_variantDictAddStr(top, TR_KEY_destination, new_path);
                printf("UPDATE: New path: %s\n", new_path);

                nb_repaired_inconsistencies++;
                free(new_path);
            } else {
                PRINT_MEMORY_ERROR()
                exit(EXIT_FAILURE);
            }
        } else {
            fprintf(stderr, "ERROR: Substring '%s' not found in '%s'\n", old, str);
        }
    }
}


void read_resume_file(tr_variant * top)
{
    /* Display informations taken from the resume file.
     * Note: This is not exhaustive.
     */

    size_t len;
    int64_t  i;
    const char * str;
    tr_variant * dict;
    bool boolVal;


    printf("\n==============================\n");
    printf("   Resume file informations   \n");
    printf("==============================\n\n");

    // Directories/files
    if ((tr_variantDictFindStr (top, TR_KEY_destination, &str, &len))
            && (str && *str))
    {
        printf("TR_KEY_destination %s\n", str);
    }

    if ((tr_variantDictFindStr (top, TR_KEY_incomplete_dir, &str, &len))
            && (str && *str))
    {
        printf("TR_KEY_incomplete_dir %s\n", str);
    }

    if (tr_variantDictFindStr (top, TR_KEY_name, &str, NULL))
    {
        printf("TR_KEY_name %s\n", str);
    }

    // DL/UP stats & state
    if (tr_variantDictFindInt (top, TR_KEY_downloaded, &i))
    {
        printf("TR_KEY_downloaded %" PRIu64 "\n", i);
    }

    if (tr_variantDictFindInt (top, TR_KEY_uploaded, &i))
    {
        printf("TR_KEY_uploaded %" PRIu64 "\n", i);
    }

    if (tr_variantDictFindBool (top, TR_KEY_paused, &boolVal))
    {
        printf("TR_KEY_paused %d\n", boolVal);
    }

    if (tr_variantDictFindInt (top, TR_KEY_seeding_time_seconds, &i))
    {
        printf("TR_KEY_seeding_time_seconds %" PRIu64 "\n", i);
    }

    if (tr_variantDictFindInt (top, TR_KEY_downloading_time_seconds, &i))
    {
        printf("TR_KEY_downloading_time_seconds %" PRIu64 "\n", i);
    }

    // Timestamped informations
    if (tr_variantDictFindInt (top, TR_KEY_added_date, &i))
    {
        printf("TR_KEY_added_date %" PRIu64 ": %s", i, ctime((time_t*)&i));
    }

    if (tr_variantDictFindInt (top, TR_KEY_done_date, &i))
    {
        printf("TR_KEY_done_date %" PRIu64 ": %s", i, ctime((time_t*)&i));
    }

    if (tr_variantDictFindInt (top, TR_KEY_activity_date, &i))
    {
        printf("TR_KEY_activity_date %" PRIu64 ": %s", i, ctime((time_t*)&i));
    }



    if (tr_variantDictFindInt (top, TR_KEY_bandwidth_priority, &i)
            /*&& tr_isPriority (i)*/)
    {
        printf("TR_KEY_bandwidth_priority %" PRIu64 "\n", i);
    }

    // Limits (speed & peers)
    if (tr_variantDictFindInt (top, TR_KEY_max_peers, &i))
    {
        printf("TR_KEY_max_peers %" PRIu64 "\n", i);
    }

    if (tr_variantDictFindDict (top, TR_KEY_speed_limit_up, &dict))
    {
        printf("Speed limit up:\n");

        if (tr_variantDictFindInt (dict, TR_KEY_speed_Bps, &i))
        {
            printf("\tTR_KEY_speed_Bps %" PRIu64 "\n", i);
        }
        else if (tr_variantDictFindInt (dict, TR_KEY_speed, &i))
            printf("\tTR_KEY_speed %" PRIu64 "\n", i*1024);

        if (tr_variantDictFindBool (dict, TR_KEY_use_speed_limit, &boolVal))
            printf("\tTR_KEY_use_speed_limit %d\n", boolVal);

        if (tr_variantDictFindBool (dict, TR_KEY_use_global_speed_limit, &boolVal))
            printf("\tTR_KEY_use_global_speed_limit %d\n", boolVal);
    }

    if (tr_variantDictFindDict (top, TR_KEY_speed_limit_down, &dict))
    {
        printf("Speed limit down:\n");

        if (tr_variantDictFindInt (dict, TR_KEY_speed_Bps, &i))
        {
            printf("\tTR_KEY_speed_Bps %" PRIu64 "\n", i);
        }
        else if (tr_variantDictFindInt (dict, TR_KEY_speed, &i))
            printf("\tTR_KEY_speed %" PRIu64 "\n", i*1024);

        if (tr_variantDictFindBool (dict, TR_KEY_use_speed_limit, &boolVal))
            printf("\tTR_KEY_use_speed_limit %d\n", boolVal);

        if (tr_variantDictFindBool (dict, TR_KEY_use_global_speed_limit, &boolVal))
            printf("\tTR_KEY_use_global_speed_limit %d\n", boolVal);
    }

    // Peers list
    const uint8_t * str_8;

    if (tr_variantDictFindRaw (top, TR_KEY_peers2, &str_8, &len))
    {
        printf("TR_KEY_peers2 %zu bytes\n", len);
    }

    if (tr_variantDictFindRaw (top, TR_KEY_peers2_6, &str_8, &len))
    {
        printf("TR_KEY_peers2_6 %zu bytes\n", len);
    }


    /* Fields not supported (yet)
     i **f (fieldsToLoad & TR_FR_PEERS)
     fieldsLoaded |= loadPeers (top, tor);

     i *f (fieldsToLoad & TR_FR_FILE_PRIORITIES)
     fieldsLoaded |= loadFilePriorities (top, tor);

     if (fieldsToLoad & TR_FR_PROGRESS)
         fieldsLoaded |= loadProgress (top, tor);

     if (fieldsToLoad & TR_FR_DND)
         fieldsLoaded |= loadDND (top, tor);

     if (fieldsToLoad & TR_FR_RATIOLIMIT)
         fieldsLoaded |= loadRatioLimits (top, tor);

     if (fieldsToLoad & TR_FR_IDLELIMIT)
         fieldsLoaded |= loadIdleLimits (top, tor);

     if (fieldsToLoad & TR_FR_FILENAMES)
         fieldsLoaded |= loadFilenames (top, tor);

     if (fieldsToLoad & TR_FR_NAME)
         fieldsLoaded |= loadName (top, tor);
     */

    /*
    tr_variant * list;

    if (tr_variantDictFindList (top, TR_KEY_files, &list))
    {
        size_t i;
        const size_t n = tr_variantListSize (list);
        printf("TR_KEY_files found\n");

        for (i=0; i<n; ++i)
        {
            //const char * str;
            size_t str_len;
            if (tr_variantGetStr (tr_variantListChild (list, i), &str, &str_len) && str && str_len)
            {
                printf("TR_KEY_files %s\n", str);
            }
        }
    }
    */
}


void repair_resume_file(tr_variant * top, char resume_filename[], bool make_changes)
{
    /* Repair entry point
     */

    printf("\n==============================\n");
    printf("        Repair attempts       \n");
    printf("==============================\n\n");


    bool force_date_update = false;
    char * full_path = NULL;

    // Get the path of downloaded files
    get_uploaded_files_path(top, &full_path);
    printf("Full path: %s\n", full_path);

    // Verify if file/directory of the torrent matches the resume filename
    check_correct_files_pointed(top, resume_filename);

    // Here we know if pointed files were ok or not (nb_repaired_inconsistencies > 0)
    // If not, we update the full path and plan to force the update of dates
    // with the dates of the new directory
    if (nb_repaired_inconsistencies > 0) {
        // Free memory
        free(full_path);

        // Get new full path
        get_uploaded_files_path(top, &full_path);
        printf("REPAIR: New full path: %s\n", full_path);

        // Force update of dates
        force_date_update = true;
    }

    // Check existence of downloaded files
    check_uploaded_files(&full_path);

    // Check dates
    check_dates(top, &full_path, force_date_update, make_changes);

    // If there are inconsistencies, the file is corrupted => cleaning step
    // Reset peers list
    if ((nb_repaired_inconsistencies > 0) && make_changes)
        reset_peers(top);

    // What was done according to make_changes value
    if (make_changes)
        printf("Repaired inconsistencies: %d\n", nb_repaired_inconsistencies);
    else
        printf("Repaired inconsistencies: 0\n");

    // Free memory
    free(full_path);
}


int main (int argc, char ** argv)
{
    char * resume_filename = NULL;
    tr_variant top;
    int err;


    if (parseCommandLine (argc, (const char**)argv))
        return EXIT_FAILURE;

    if (showVersion)
    {
        fprintf(stderr, MY_NAME" "LONG_VERSION_STRING"\n");
        return EXIT_SUCCESS;
    }

    if (resume_file == NULL)
    {
        fprintf (stderr, "ERROR: No resume file specified.\n");
        tr_getopt_usage (MY_NAME, getUsage (), options);
        fprintf (stderr, "\n");
        return EXIT_FAILURE;
    }


    // Load the resume file in memory
    if (tr_variantFromFile (&top, TR_VARIANT_FMT_BENC, resume_file))
    {
        fprintf(stderr, "ERROR: Resume file could not be opened !\n");
        exit(EXIT_FAILURE);
    }


    // Load data from resume file & show parameters (verbose mode)
    if (verbose) {
        printf("Parameters: show version: %d, make changes: %d, resume file: %s,  replace old: %s, replace new: %s\n",
               showVersion, make_changes, resume_file, replace[0], replace[1]);
        read_resume_file(&top);
    }

    // Repair or replace directory ?
    if (replace[0] == NULL) {
        // Get the basename of the resume file
        resume_filename = basename((char*)resume_file); // cast to non-const
        //printf("resume file: %s\n", resume_filename);

        // Repair attempts
        repair_resume_file(&top, resume_filename, make_changes);
    } else {
        // Replace directory
        replace_dir(&top, replace[0], replace[1]);
    }


    // Write the resume file if inconsistencies are repaired, and if changes are allowed
    if (nb_repaired_inconsistencies > 0 && make_changes)
    {
        if ((err = tr_variantToFile(&top, TR_VARIANT_FMT_BENC, resume_file)))
            fprintf(stderr, "ERROR: While saving the new .resume file\n");
        else
            printf("The file was successfully modified.\n");

    } else {
        printf("The file remains untouched.\n");
    }

    // Free memory
    tr_variantFree (&top);
    exit(EXIT_SUCCESS);
}
