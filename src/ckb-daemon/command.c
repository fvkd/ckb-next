#include "command.h"
#include "device.h"
#include "devnode.h"
#include "led.h"
#include "notify.h"
#include "profile.h"
#include "usb.h"

static const char* const cmd_strings[CMD_COUNT - 1] = {
    // NONE is implicit
    "mode",
    "switch",
    "layout",
    "notifyon",
    "notifyoff",

    "hwload",
    "hwsave",
    "fwupdate",

    "active",
    "idle",

    "erase",
    "eraseprofile",
    "name",
    "profilename",
    "id",
    "profileid",

    "rgb",
    "ioff",
    "ion",
    "iauto",

    "bind",
    "unbind",
    "rebind",
    "macro",

    "notify",
    "inotify",
    "get"
};

#define TRY_WITH_RESET(action)  \
    while(action){              \
        if(usb_tryreset(kb)){   \
            free(word);         \
            return 1;           \
        }                       \
    }


int readcmd(usbdevice* kb, const char* line){
    char* word = malloc(strlen(line) + 1);
    int wordlen;
    const char* newline = 0;
    const devcmd* vt = kb->vtable;
    kbprofile* profile = kb->profile;
    usbmode* mode = 0;
    int notifynumber = 0;
    // Read words from the input
    cmd command = NONE;
    while(sscanf(line, "%s%n", word, &wordlen) == 1){
        line += wordlen;
        // If we passed a newline, reset the context
        if(line > newline){
            mode = profile->currentmode;
            command = NONE;
            notifynumber = 0;
            newline = strchr(line, '\n');
            if(!newline)
                newline = line + strlen(line);
        }
        // Check for a command word
        for(int i = 0; i < CMD_COUNT - 1; i++){
            if(!strcmp(word, cmd_strings[i])){
                command = i + CMD_FIRST;
#ifndef OS_MAC
                // Layout command isn't used on Linux; ignore
                if(command == LAYOUT)
                    command = NONE;
#endif
                // Most commands require parameters, but a few are actions in and of themselves
                if(command != SWITCH
                        && command != HWLOAD && command != HWSAVE
                        && command != ACTIVE && command != IDLE
                        && command != ERASE && command != ERASEPROFILE)
                    goto next_loop;
                break;
            }
        }

        // Set current notification node when given @number
        int newnotify;
        if(sscanf(word, "@%u", &newnotify) == 1 && newnotify < OUTFIFO_MAX){
            notifynumber = newnotify;
            continue;
        }

        // Reject unrecognized commands. Reject bind or notify related commands if the keyboard doesn't have the feature enabled.
        if(command == NONE
                || ((!HAS_FEATURES(kb, FEAT_BIND) && (command == BIND || command == UNBIND || command == REBIND || command == MACRO))
                           || (!HAS_FEATURES(kb, FEAT_NOTIFY) && command == NOTIFY))){
            next_loop:
            continue;
        }
        // Reject anything not related to fwupdate if device has a bricked FW
        if(NEEDS_FW_UPDATE(kb) && command != FWUPDATE && command != NOTIFYON && command != NOTIFYOFF)
            continue;

        // Specially handled commands - these are available even when keyboard is IDLE
        else if(command == NOTIFYON){
            int notify;
            if(sscanf(word, "%u", &notify) == 1)
                mknotifynode(kb, notify);
            continue;
        } else if(command == NOTIFYOFF){
            int notify;
            if(sscanf(word, "%u", &notify) == 1 && notify != 0)
                rmnotifynode(kb, notify);
            continue;
        } else if(command == GET){
            vt->get(kb, mode, notifynumber, 0, word);
            continue;
        } else if(command == LAYOUT){
            if(!strcmp(word, "ansi"))
                kb->features = (kb->features & ~FEAT_LMASK) | FEAT_ANSI;
            else if(!strcmp(word, "iso"))
                kb->features = (kb->features & ~FEAT_LMASK) | FEAT_ISO;
        }
        // If a keyboard is inactive, it must be activated before receiving any other commands
        if(!kb->active){
            if(command == ACTIVE)
                TRY_WITH_RESET(vt->active(kb, mode, notifynumber, 0, 0));
            continue;
        }
        // Specially handled commands only available when keyboard is ACTIVE
        switch(command){
        case IDLE:
            TRY_WITH_RESET(vt->idle(kb, mode, notifynumber, 0, 0));
            continue;
        case MODE: {
            // Mode selection processes a number
            int newmode;
            if(sscanf(word, "%u", &newmode) == 1 && newmode > 0 && newmode <= MODE_COUNT)
                mode = profile->mode + newmode - 1;
            continue;
        } case SWITCH:
            profile->currentmode = mode;
            // Set mode light for non-RGB K95
            int index = INDEX_OF(mode, profile->mode);
            vt->setmodeindex(kb, index);
            continue;
        case HWLOAD: case HWSAVE:
            // Try to load/save the hardware profile. Reset on failure, disconnect if reset fails.
            TRY_WITH_RESET(vt->do_io[command](kb, mode, notifynumber, 1, 0));
            // Re-send the current RGB state as it sometimes gets scrambled
            TRY_WITH_RESET(vt->updatergb(kb, 1));
            continue;
        case FWUPDATE:
            // FW update parses a whole word. Unlike hwload/hwsave, there's no try again on failure.
            if(vt->fwupdate(kb, mode, notifynumber, 0, word)){
                free(word);
                return 1;
            }
            continue;
        case ERASEPROFILE:
            // Erase the current profile
            vt->eraseprofile(kb, mode, notifynumber, 0, 0);
            // Update profile/mode pointers
            profile = kb->profile;
            mode = profile->currentmode;
            continue;
        case ERASE: case NAME: case IOFF: case ION: case IAUTO: case INOTIFY: case PROFILENAME: case ID: case PROFILEID:
            // All of the above just parse the whole word
            vt->do_cmd[command](kb, mode, notifynumber, 0, word);
            continue;
        case RGB: {
            // RGB command has a special response for a single hex constant
            int r, g, b;
            if(sscanf(word, "%02x%02x%02x", &r, &g, &b) == 3){
                // Set all keys
                for(int i = 0; i < N_KEYS_EXTENDED; i++)
                    vt->rgb(kb, mode, notifynumber, i, word);
                continue;
            }
            break;
        } case MACRO:
            if(!strcmp(word, "clear")){
                // Macro has a special clear command
                vt->macro(kb, mode, notifynumber, 0, 0);
                continue;
            }
            break;
        default:;
        }
        // For anything else, split the parameter at the colon
        int left = -1;
        sscanf(word, "%*[^:]%n", &left);
        if(left <= 0)
            continue;
        const char* right = word + left;
        if(right[0] == ':')
            right++;
        // Macros have a separate left-side handler
        if(command == MACRO){
            word[left] = 0;
            vt->macro(kb, mode, notifynumber, word, right);
            continue;
        }
        // Scan the left side for key names and run the requested command
        int position = 0, field = 0;
        char keyname[11];
        while(position < left && sscanf(word + position, "%10[^:,]%n", keyname, &field) == 1){
            int keycode;
            if(!strcmp(keyname, "all")){
                // Set all keys
                for(int i = 0; i < N_KEYS_EXTENDED; i++)
                    vt->do_cmd[command](kb, mode, notifynumber, i, right);
            } else if((sscanf(keyname, "#%d", &keycode) && keycode >= 0 && keycode < N_KEYS_EXTENDED)
                      || (sscanf(keyname, "#x%x", &keycode) && keycode >= 0 && keycode < N_KEYS_EXTENDED)){
                // Set a key numerically
                vt->do_cmd[command](kb, mode, notifynumber, keycode, right);
            } else {
                // Find this key in the keymap
                for(unsigned i = 0; i < N_KEYS_EXTENDED; i++){
                    if(keymap[i].name && !strcmp(keyname, keymap[i].name)){
                        vt->do_cmd[command](kb, mode, notifynumber, i, right);
                        break;
                    }
                }
            }
            if(word[position += field] == ',')
                position++;
        }
    }

    // Finish up
    if(!NEEDS_FW_UPDATE(kb))
        TRY_WITH_RESET(vt->updatergb(kb, 0));
    free(word);
    return 0;
}