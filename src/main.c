/** @file
 * Main entry to the android logger software.
 *
 *==============================================================================
 * Copyright 2013 by Brandon Edens. All Rights Reserved
 *==============================================================================
 *
 * android-log is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * android-log is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with android-log. If not, see <http://www.gnu.org/licenses/>.
 *
 * @author  Brandon Edens
 * @date    2013-08-14
 * @details
 *
 * This file implements software for colorizing and reporting the logs of all
 * Android devices attached to the host. The color output of this logging is
 * similar to Jeff Sharkey's Python software. However, unlike that software,
 * this software will spawn multiple threads of execution attempting to
 * continuously update the notion of available Android devices and automatically
 * attempting to log their messages.
 */

/*******************************************************************************
 * Include Files
 */
#include <assert.h>
#include <pthread.h>
#include <sys/types.h>
#include <regex.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ccan/strmap/strmap.h>

/*******************************************************************************
 * Constants
 */

/** Maximum number of characters for command. */
#define COMMAND_NCHARS (128)

/** Number of seconds to delay between attempts to find devices. */
#define DELAY_BETWEEN_DEVICE_CHECK (3)

/** Maximum number of characters for device name. */
#define DEVICE_NAME_NCHARS (64)

/** Maxmium number of characters in a read line. */
#define LINE_NCHARS (1024)

/**
 * When matching line of device text we expect whole string to match and the
 * substring that is the device's name/serial.
 */
#define MAX_MATCHES (2)

/** Maximum number of retries on starting logcat execution. */
#define RETRIES_NMAX (10)

/** Maximum number of characters allowed in Android device serial number. */
#define SERIAL_NCHARS (128)

/** Shell command max number of characters. */
#define SHELL_NCHARS (128)

/*******************************************************************************
 * Local Types
 */

/** Listing of colors we have available in our console. */
enum color {
    COLOR_RED = 0,
    COLOR_GREEN,
    COLOR_YELLOW,
    COLOR_BLUE,
    COLOR_MAGENTA,
    COLOR_CYAN,
    COLOR_BRIGHT_RED,
    COLOR_BRIGHT_GREEN,
    COLOR_BRIGHT_YELLOW,
    COLOR_BRIGHT_BLUE,
    COLOR_BRIGHT_MAGENTA,
    COLOR_BRIGHT_CYAN,
    COLOR_NMAX
};

/**
 * The index numbers for different portions of matched regular expression in
 * line of log.
 */
enum regex_match {
    WHOLE = 0,
    TIME,
    TAGTYPE,
    TAG,
    OWNER,
    MESSAGE,
    MESSAGE_NPARTS
};

/**
 * Representation of an Android device connected to the host.
 */
struct device {
    pthread_t  thread;              //!< Thread of execution for device.
    char       name[SERIAL_NCHARS]; //!< Serial number of device.
    FILE      *fh;                  //!< Handle to running process.
    enum color color;               //!< Color of the device's name.
};

/*******************************************************************************
 * Local Variables
 */

// Table that converts a color code to its respective ANSI.
static uint8_t color_ansi_table[] = {
    31, // RED
    32, // GREEN
    33, // YELLOW
    34, // BLUE
    35, // MAGENTA
    36, // CYAN
    91, // BRIGHT_RED
    92, // BRIGHT_GREEN
    93, // BRIGHT_YELLOW
    94, // BRIGHT_BLUE
    95, // BRIGHT_MAGENTA
    96  // BRIGHT_CYAN
};


/** Map of device names to device struct. */
static struct { STRMAP_MEMBERS(struct device *); } device_map;

/** Lock used to prevent concurrent read and write. */
static pthread_mutex_t device_map_lock = PTHREAD_MUTEX_INITIALIZER;

/** Lock used to prevent concurrent writing of line of text to display. */
static pthread_mutex_t line_lock = PTHREAD_MUTEX_INITIALIZER;

/** Flag that indicates whether or not we are to shutdown software. */
static bool shutdown = false;

/** Map of tag names to colors used in color output. */
static struct { STRMAP_MEMBERS(enum color *); } tag_map;

/** Lock used to prevent concurrent modification / access of tag map. */
static pthread_mutex_t tag_map_lock = PTHREAD_MUTEX_INITIALIZER;

/*******************************************************************************
 * Local Functions
 */

static void find_android_devices(void);
static bool handle_count_member(const char *member, struct device *device,
                                void *count);
static bool handle_delete_color(const char *member, enum color *color,
                                void *unused);
static char *parse_match(regmatch_t *match, char *in,
                         char *out, size_t out_len);
static void *run_find_devices(void *unused);
static void *run_logcat(void *device);

/******************************************************************************/

int main(int argc, char *argv[])
{
    // Setup software.
    pthread_t device_mon;
    strmap_init(&device_map);
    strmap_init(&tag_map);

    // Fill out the colors used for tag names.
    enum color *c = malloc(sizeof(*c));
    *c = COLOR_BLUE;
    strmap_add(&tag_map, "dalvikvm", c);
    strmap_add(&tag_map, "Process", c);
    c = malloc(sizeof(*c));
    *c = COLOR_CYAN;
    strmap_add(&tag_map, "ActivityManager", c);
    strmap_add(&tag_map, "ActivityThread", c);

    // Start thread of execution that will periodically check on available
    // android devices.
    pthread_create(&device_mon, NULL, run_find_devices, NULL);

    // Slight delay while we wait for first pass of devices to load.
    sleep(1);

    // Count the number of devices we discovered.
    int count = 0;
    pthread_mutex_lock(&device_map_lock);
    strmap_iterate(&device_map, handle_count_member, &count);
    pthread_mutex_unlock(&device_map_lock);
    if (count == 0) {
        fprintf(stderr, "Waiting on device to connect.\n");
    }
    pthread_join(device_mon, NULL);

    // Delete all colors out of the color map.
    strmap_iterate(&tag_map, handle_delete_color, NULL);
    strmap_clear(&tag_map);
    return 0;
}

/**
 * Recover list of android devices. This function queries adb devices to
 * determine what Android devices are connected to the host. Devices found are
 * added to the global devices set of strings.
 */
static void find_android_devices(void)
{
    int err;
    static enum color next_color;
    static pthread_mutex_t next_color_lock = PTHREAD_MUTEX_INITIALIZER;

    char cmd[COMMAND_NCHARS];
    snprintf(cmd, sizeof(cmd), "adb devices");
    FILE *file = popen(cmd, "r");

    regex_t preg;
    err = regcomp(&preg, "^([0-9A-Fa-f]+)[ \t]+device.*$", REG_EXTENDED);
    assert(!err);

    char line[LINE_NCHARS];
    while (fgets(line, sizeof(line), file) != NULL) {
        regmatch_t matches[MAX_MATCHES];
        err = regexec(&preg, line, MAX_MATCHES, matches, 0);
        if (err == REG_NOMATCH) {
            // Failure to find matching line. Continue.
            continue;
        }

        size_t start = matches[1].rm_so;
        size_t len   = matches[1].rm_eo - start;
        if (len == 0) {
            fprintf(stderr, "Could not find start / end of match.\n");
            continue;
        }

        // Check if we already know of this device.
        char name[DEVICE_NAME_NCHARS];
        memset(name, 0, sizeof(name));
        strncpy(name, &line[start], len);

        pthread_mutex_lock(&device_map_lock);
        if (strmap_get(&device_map, name) != NULL) {
            pthread_mutex_unlock(&device_map_lock);
            // Name already in the map.
            continue;
        }
        pthread_mutex_unlock(&device_map_lock);

        // Lets create the device and add it to the map.
        struct device *device = (struct device *)malloc(sizeof(struct device));
        memset(device->name, 0, sizeof(device->name));
        strcpy(device->name, name);
        // Set up the device's color.
        pthread_mutex_lock(&next_color_lock);
        device->color = next_color;
        ++next_color;
        if (next_color == COLOR_NMAX) {
            next_color = COLOR_RED;
        }
        pthread_mutex_unlock(&next_color_lock);
        // Add device to the device map.
        pthread_mutex_lock(&device_map_lock);
        strmap_add(&device_map, device->name, device);
        pthread_mutex_unlock(&device_map_lock);

        // Begin running logcat.
        err = pthread_create(&device->thread, NULL, run_logcat, device);
        assert(!err);
    }
    regfree(&preg);
}

/**
 * Handler that counts the numer of members by iterating the count each time its
 * called recursively.
 */
static bool handle_count_member(const char *member, struct device *device,
                                void *count)
{
    int *c = (int *)count;
    if (member != NULL) {
        ++(*c);
    }
    return true;
}

/**
 * Handler that deletes the given color that corresponds to member.
 */
static bool handle_delete_color(const char *member, enum color *color,
                                void *unused)
{
    free(color);
    return true;
}

/**
 * Extracts the text for a match in the regular expression from the given input
 * text and store the extracted text into the out variable.
 */
static char *parse_match(regmatch_t *match, char *in,
                         char *out, size_t out_len)
{
    memset(out, 0, out_len);
    size_t len = match->rm_eo - match->rm_so;
    strncpy(out, &in[match->rm_so], len);
    return out;
}

/**
 * Run thread of execution that continuously polls adb for listing of connected
 * devices updating our set of known devices.
 */
static void *run_find_devices(void *unused)
{
    while (!shutdown) {
        find_android_devices();
        sleep(DELAY_BETWEEN_DEVICE_CHECK);
    }
    return NULL;
}

/**
 * Run logcat for the given device.
 */
static void *run_logcat(void *device)
{
    int err;

    struct device *d = (struct device *)device;
    char cmd[SHELL_NCHARS];
    snprintf(cmd, sizeof(cmd), "adb -s %s logcat -v time", d->name);
    d->fh = popen(cmd, "r");
    int retries = 0;
    do {
        d->fh = popen(cmd, "r");
        if (d->fh == NULL) {
            fprintf(stderr, "Failure to open device\n");
            ++retries;
            if (retries > RETRIES_NMAX) {
                fprintf(stderr, "Failure to start logcat for device: %s\n",
                        d->name);
                return NULL;
            }
            sleep(1);
        }
    } while (d->fh == NULL);

    regex_t preg;
    err = regcomp(&preg, "^([0-9]{2}-[0-9]{2} "
                         "[0-9]{2}:[0-9]{2}:[0-9]{2}\\.[0-9]{3}) "
                         "([A-Z])/([^\\(]+)\\(([^\\)]+)\\): (.*)$",
                  REG_EXTENDED);
    assert(!err);

    char line[LINE_NCHARS];
    while (!shutdown && fgets(line, sizeof(line), d->fh) != NULL) {
        // Continuously read log lines and print them.
        regmatch_t matches[MESSAGE_NPARTS];
        err = regexec(&preg, line, MESSAGE_NPARTS, matches, 0);
        if (err == REG_NOMATCH) {
            fprintf(stderr, "Received line that did not match pattern: %s.\n",
                    line);
            continue;
        }

        // Line that accumulates the color log messages.
        char log[LINE_NCHARS];
        memset(log, 0, sizeof(log));

        // Text that houses portion of regular expression match.
        char match[LINE_NCHARS];

        // Print device name.
        size_t len = sprintf(log, "\e[%dm%-16.16s\e[0m",
                             color_ansi_table[d->color],
                             d->name);

        // Print the time of the logged message.
        len += sprintf(log + len, " \e[34m%s\e[0m",
                       parse_match(&matches[TIME], line,
                                   match, sizeof(match)));

        // Print the owner of the message.
        len += sprintf(log + len, " \e[30;100m%s\e[0m",
                       parse_match(&matches[OWNER], line,
                                   match, sizeof(match)));

        // Print the tag.
        // The color we're currently using for a message.
        static enum color next_color;
        static pthread_mutex_t color_lock = PTHREAD_MUTEX_INITIALIZER;

        parse_match(&matches[TAG], line, match, sizeof(match));
        pthread_mutex_lock(&tag_map_lock);
        enum color *color = strmap_get(&tag_map, match);
        pthread_mutex_unlock(&tag_map_lock);

        if (color == NULL) {
            color = (enum color *)malloc(sizeof(*color));

            pthread_mutex_lock(&color_lock);
            *color = next_color;
            // Update our color after we've assigned its value.
            ++next_color;
            if (next_color == COLOR_NMAX) {
                next_color = COLOR_RED;
            }
            pthread_mutex_unlock(&color_lock);

            // Tag does not already exists therefore we add it.
            pthread_mutex_lock(&tag_map_lock);
            strmap_add(&tag_map, match, color);
            pthread_mutex_unlock(&tag_map_lock);

        } else {
            pthread_mutex_unlock(&tag_map_lock);
        }
        len += sprintf(log + len, " \e[%dm%-20.20s\e[0m",
                       color_ansi_table[*color], match);

        // print tagtype
        len += sprintf(log + len, " ");
        parse_match(&matches[TAGTYPE], line, match, sizeof(match));
        switch (*match) {
        case 'D':
            len += sprintf(log + len, "\e[30;44m D \e[0m");
            break;
        case 'E':
            len += sprintf(log + len, "\e[30;41m E \e[0m");
            break;
        case 'F':
            len += sprintf(log + len, "\e[5;30;41m F \e[0m");
            break;
        case 'I':
            len += sprintf(log + len, "\e[30;42m I \e[0m");
            break;
        case 'V':
            len += sprintf(log + len, "\e[37m V ");
            break;
        case 'W':
            len += sprintf(log + len, "\e[30;43m W \e[0m");
            break;
        }

        // print message
        len += sprintf(log + len, " \e[1;30m%s\e[0m",
                       parse_match(&matches[MESSAGE], line,
                                   match, sizeof(match)));

        pthread_mutex_lock(&line_lock);
        fputs(log, stdout);
        pthread_mutex_unlock(&line_lock);
    }

    // Device disconnected; cleanup the device resources.
    pthread_mutex_lock(&device_map_lock);
    strmap_del(&device_map, d->name, NULL);
    pthread_mutex_unlock(&device_map_lock);
    free(d);

    return NULL;
}

