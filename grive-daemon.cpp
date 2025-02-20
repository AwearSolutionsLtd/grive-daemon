/*
grive-daemon syncs your ~/Google Drive folder by calling grive.
Copyright (C) 2014  Christopher Kyle Horton

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

/* Programmed with help from this StackExchange answer:
   http://stackoverflow.com/a/17955149
   Also, from this blog post:
   http://darkeside.blogspot.com/2007/12/linux-inotify-example.html
   And this:
   http://www.ibm.com/developerworks/linux/library/l-ubuntu-inotify/index.html
   And this (for recursive directory watching):
   https://gist.github.com/pkrnjevic/6016356
   And this too, for more recursive directory watching:
   http://stackoverflow.com/a/11097815
   */

#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <syslog.h>
#include <pwd.h>
#include <string.h>
#include <errno.h>
#include <sys/inotify.h>
#include "daemon.h"
#include "watch.h"
#include "recursive_watch.h"

/* Allow for 1024 simultaneous events */
#define BUFF_SIZE ((sizeof(struct inotify_event)+FILENAME_MAX)*1024)

static bool get_event (int fd, const char * target, Watch& watch)
{
  ssize_t len, i = 0;
  char action[81+FILENAME_MAX] = {0};
  char buff[BUFF_SIZE] = {0};
  string current_dir, new_dir;
  bool sync_required = false;
  int wd;

  len = read (fd, buff, BUFF_SIZE);

  while (i < len) {
    struct inotify_event *pevent = (struct inotify_event *)&buff[i];
    char action[81+FILENAME_MAX] = {0};
    char system_message[200+FILENAME_MAX] = {0};
    bool relevant_change = false;

    if (pevent->len) 
       strcpy (action, pevent->name);
    else
       strcpy (action, target);
    
    if (pevent->mask & IN_ATTRIB) 
       strcat(action, "'s metadata changed but this is ignored");
    if (pevent->mask & IN_CREATE)
    {
        current_dir = watch.get(pevent->wd);
        if (pevent->mask & IN_ISDIR)
        {
          new_dir = current_dir + "/" + pevent->name;
          wd = inotify_add_watch(fd, new_dir.c_str(), WATCH_FLAGS);
          watch.insert(pevent->wd, pevent->name, wd);
          //printf("New directory %s created.\n", new_dir.c_str());
        } else {
          //printf("New file %s/%s created.\n", current_dir.c_str(), pevent->name);
        }
        strcat(action, " was created in a watched directory");
        relevant_change = true;
    }
    if (pevent->mask & IN_DELETE)
    {
        if (pevent->mask & IN_ISDIR) {
            new_dir = watch.erase(pevent->wd, pevent->name, &wd);
            inotify_rm_watch(fd, wd);
            //printf( "Directory %s deleted.\n", new_dir.c_str() );
        } else {
            current_dir = watch.get(pevent->wd);
            //printf( "File %s/%s deleted.\n", current_dir.c_str(), pevent->name );
        }
        strcat(action, " was deleted in a watched directory");
        relevant_change = true;
    }
    if (pevent->mask & IN_DELETE_SELF) {
       strcat(action, ", the watched file/directory, was itself deleted");
       relevant_change = true;
    }
    if (pevent->mask & IN_MODIFY) {
       strcat(action, " was modified");
       relevant_change = true;
    }
    if (pevent->mask & IN_MOVE_SELF) {
       strcat(action, ", the watched file/directory, was itself moved");
       relevant_change = true;
    }
    if (pevent->mask & IN_MOVED_FROM) {
       strcat(action, " was moved out of a watched directory");
       relevant_change = true;
    }
    if (pevent->mask & IN_MOVED_TO) {
       strcat(action, " was moved into a watched directory");
       relevant_change = true;
    }
    
    // Ignore hidden grive files.
    if (pevent->len) {
      if ((strcmp (pevent->name, ".grive") != 0) && (strcmp (pevent->name, ".grive_state") != 0)) {
        syslog (LOG_INFO, "Noticed %s.", action);
        while (true) {
          // Replace dangerous char
          char *char_ptr = strchr(action, '"');
          if (!char_ptr) {
            break;
          }
          *char_ptr = '_';
        }
        while (true) {
          // Replace dangerous char
          char *char_ptr = strchr(action, '$');
          if (!char_ptr) {
            break;
          }
          *char_ptr = '_';
        }
        while (true) {
          // Replace dangerous char
          char *char_ptr = strchr(action, '\'');
          if (!char_ptr) {
            break;
          }
          *char_ptr = '_';
        }
        strcat(system_message, "notify-send \"Grive Daemon: \" \"");
        strcat(system_message, action);
        strcat(system_message, "\"");
        system (system_message);
        if (relevant_change) {
          sync_required = true;
        }
      }
    }
    
    i += sizeof(struct inotify_event) + pevent->len;
  }
  
  return sync_required;
}

void handle_error (int error)
{
  syslog (LOG_WARNING, "Needed to handle error %s.", strerror(error));
  fprintf (stderr, "Error: %s\n", strerror(error));
}

int main()
{
    Watch watch;
    char target[FILENAME_MAX];
    int result, fd, wd;
    struct passwd *pw = getpwuid(getuid());
    char *gd_dir = strcat(pw->pw_dir, "/Google Drive");
    
    daemonize();
    
    syslog (LOG_NOTICE, "Started.");
    
    if (chdir(gd_dir) < 0)
    {
        syslog (LOG_WARNING, "Could not switch to ~/Google Drive.");
        system ("notify-send \"Google Drive sync failed\" \"grive-daemon could not switch to ~/Google Drive.\"");
        exit(EXIT_FAILURE);
    }
    
    // Initial syncronization in case stuff changed before starting.
    system ("notify-send \"Google Drive sync started\" \"Ensuring both local and remote are up-to-date...\"");
    system ("grive");
    syslog (LOG_INFO, "Performed an initial syncronization.");
    system ("notify-send \"Google Drive sync complete\" \"Local and remote are now both synced.\"");
    
    fd = inotify_init();
    if (fd < 0) {
      handle_error (errno);
      return EXIT_FAILURE;
    }

    wd = inotify_add_watch (fd, gd_dir, WATCH_FLAGS);
    if (wd < 0) {
      syslog (LOG_WARNING, "gd_dir = %s.", gd_dir);
      handle_error (errno);
      system ("notify-send \"Google Drive sync failed\" \"grive-daemon encountered an error and exited.\"");
      return EXIT_FAILURE;
    }
    watch.insert(-1, gd_dir, wd);
    
    DirectoryReader::parseDirectory(string(gd_dir), fd, watch);
    
    while (1)
    {
        if (get_event(fd, target, watch))
        {
          system ("notify-send \"Google Drive sync started\" \"Syncing recent local changes with remote...\"");
          system ("grive");
          syslog (LOG_INFO, "Performed a syncronization.");
          system ("notify-send \"Google Drive sync complete\" \"Remote is now up-to-date with local changes.\"");
        }
        sleep (2);
    }
    
    inotify_rm_watch(fd, wd);
    watch.cleanup(fd);
    close(fd);
    system ("notify-send \"Google Drive sync stopped\" \"grive-daemon was terminated. Your changes will not sync until it is restarted.\"");
    syslog (LOG_NOTICE, "Terminated.");
    closelog();

    return EXIT_SUCCESS;
}
