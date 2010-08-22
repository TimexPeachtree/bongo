/****************************************************************************
 * <Novell-copyright>
 * Copyright (c) 2005, 2006 Novell, Inc. All Rights Reserved.
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General Public License
 * as published by the Free Software Foundation.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, contact Novell, Inc.
 * 
 * To contact Novell about this file by physical or electronic mail, you 
 * may find current contact information at www.novell.com.
 * </Novell-copyright>
 ****************************************************************************/
// Parts Copyright (C) 2007 Alex Hudson. See COPYING for details.

#include <config.h>
#include <xpl.h>
#include <bongoutil.h>
#include <assert.h>
#include <limits.h>
#include <time.h>

#include "stored.h"

#include "messages.h"
#include "mime.h"
#include "mail.h"
#include "calendar.h"
#include "contacts.h"

int
IsOwnStoreSelected(StoreClient *client)
{
    return (STORE_PRINCIPAL_USER == client->principal.type && 
            (!strcmp(client->principal.name, client->storeName)));
}

/* Creates a new collection, including any ancestors required.  Also touches the file
   for this collection (but doesn't truncate it, since this function is also used by
   the RENAME command (which is a bit of an abstraction violation))

   returns:  0 success
            -1 bad collection name
            -2 db lib err
            -3 guid exists
            -4 io error
            -5 permission denied
*/

int
StoreCreateCollection(StoreClient *client, char *name, uint64_t guid,
                      uint64_t *outGuid, int32_t *outVersion)
{
    StoreObject new_collection;
    char *p;
    int res;
    int namelen = strlen(name);

    /* validate collection name */
    if ('/' != *name ||
        namelen < 2 ||
        namelen > STORE_MAX_COLLECTION - 1 ||
        '/' == name[namelen-1])
    {
        return -1;
    }

    memset (&new_collection, 0, sizeof(StoreObject));
    new_collection.type = STORE_DOCTYPE_FOLDER;
    new_collection.collection_guid = 1; /* root guid */
    
	if (guid)
		new_collection.guid = guid;	// check at some point?

    p = name;
    do {
    	// FIXME: recursively create any collections in our path which don't exist yet
    	StoreObject collection;
    	collection.guid = 0;

/*    	
    	collection.guid = 0;
        *p++ = '/';
        p = strchr(p, '/');
        if (p)
        	*p = 0;
        else
        	collection.guid = guid;
        
           
        dcode = DStoreFindCollectionGuid(client->handle, name, &info.collection);
        if (-1 == dcode) {
            return -2;
        } else if (1 == dcode) {
            if (!p) {
                return -3;
            } else {
                continue;
            }
        }

        if (StoreCheckAuthorizationGuidQuiet(client, info.collection, 
                                             STORE_PRIV_BIND)) 
        {
            return -5;
        }

        strncpy (info.filename, name, sizeof(info.filename) - 1);

        if (DStoreSetDocInfo(client->handle, &info)) {
            return -2;
        }
        info.collection = info.guid;
*/
        res = MaildirNew(client->store, collection.guid);
        if (res != 0) return res;
    } while (p);

    if (outGuid) *outGuid = new_collection.guid;
    if (outVersion) *outVersion = new_collection.version;
    
    return 0;
}
                 
/* find the user's store path, creating it if necessary 
   path must be a buffer of size XPL_MAX_PATH + 1 or greater
   returns 0 on success, -1 o/w
*/

int
SetupStore(const char *user, const char **storeRoot, char *path, size_t len)
{
	int n;
	struct stat sb;
	BOOL make_store = FALSE;

	n = snprintf(path, len, "%s/%s/", StoreAgent.store.rootDir, user);
	path[len] = 0;
	path[n-1] = 0;

	if (stat(path, &sb)) {
		if (XplMakeDir(path)) {
			Log(LOG_ERROR, "Error creating store directory: %s.", strerror(errno));
			return -1;
		}
		make_store = TRUE;
	}
	
	path[n-1] = '/';

	if (make_store) {
		uint64_t i;
		for (i = 0x2; i < 0xe; i++) {
			if (MaildirNew(path, i)) {
				Log(LOG_ERROR, "Error creating store: %s.", strerror(errno));
				return -1;
			}
		}
	}
	path[n] = 0;

	*storeRoot = NULL;

	return 0;
}


/* This function gets called after a document has been added/replaced to the store,
   before committing the transaction

   NOTE: info->data fields are not valid after this function has been called.
*/

const char *
StoreProcessDocument(StoreClient *client, 
                     StoreObject *document,
                     const char *path)
{
	const char *result = NULL;
	void *mark = BongoMemStackPeek(&client->memstack);

	/* type-dependent processing */
	switch (document->type) {
		case STORE_DOCTYPE_MAIL:
			result = StoreProcessIncomingMail(client, document, path);
			break;
		case STORE_DOCTYPE_EVENT:
			// FIXME: how do we link this into a calendar automatically?
			result = StoreProcessIncomingEvent(client, document, 0);
			break;
		case STORE_DOCTYPE_CONTACT:
			result = StoreProcessIncomingContact(client, document, path);
			break;
		default:
			break;
	}

	BongoMemStackPop(&client->memstack, mark);

	return result;
}

/* login */
CCode
SelectUser(StoreClient *client, char *user, char *password, int nouser)
{
    CCode ccode = -1;
    char buf[INET_ADDRSTRLEN+1];

    if (StoreAgent.installMode) {
        // Don't let users login in install mode.
        // FIXME: a better error message? 
        ccode = ConnWriteStr(client->conn, MSG4224NOUSER);
        goto finish;
    }

    if (0 != MsgAuthFindUser(user)) {
        if (IS_MANAGER(client)) {
            ccode = ConnWriteStr(client->conn, MSG4224NOUSER);
        } else {
            ccode = ConnWriteStr(client->conn, MSG3242BADAUTH);
            XplDelay(2000);
        }
        goto finish;
    }

    if (password && MsgAuthVerifyPassword(user, password) != 0) {
        ccode = ConnWriteStr(client->conn, MSG3242BADAUTH);
        XplDelay(2000);
        goto finish;
    }

    // FIXME: I think we're supposed to refer to the correct store IP?
    strncpy(buf, "127.0.0.1", INET_ADDRSTRLEN);
    ccode = ConnWriteF(client->conn, "1000 %s\r\n", buf);

    if (nouser) {
        goto finish;
    }

    UnselectUser(client);

    client->principal.type = STORE_PRINCIPAL_USER;
    strncpy(client->principal.name, user, sizeof(client->principal.name));
    client->principal.name[sizeof(client->principal.name) - 1] = 0;
    client->flags |= STORE_CLIENT_FLAG_IDENTITY;

finish:
    return ccode;
}

/* opens the store for the given user */
/* returns: -1 on error */

int 
SelectStore(StoreClient *client, char *user)
{
	const char *storeRoot = NULL;
	char path[XPL_MAX_PATH + 1];
	struct stat sb;
	
	// check if we already have this store selected
	if (client->storeName && !strcmp(user, client->storeName)) {
		return 0;
	}
	
	// close current selected store if necessary
	UnselectStore(client);
	
	snprintf(path, XPL_MAX_PATH, "%s/%s/", StoreAgent.store.rootDir, user);
	strcpy(client->store, path);
	
	client->storeRoot = storeRoot;
	client->storeHash = BongoStringHash(user);
	client->storeName = MemStrdup(user);
	
	client->stats.insertions = 0;
	client->stats.updates = 0;
	client->stats.deletions = 0;
	
	if (stat(path, &sb)) {
		// this store hasn't been created yet
		if (XplMakeDir(path)) {
			Log(LOG_ERROR, "Error creating store directory: %s.", strerror(errno));
			return -1;
		}
		if (StoreDBOpen(client, user)) {
			Log(LOG_ERROR, "Couldn't open store database for %s", user);
			return -2;
		}
		if (StoreObjectDBCreate(client)) {
			Log(LOG_ERROR, "Couldn't setup initial store database for %s", user);
			return -3;
		}
	} else {
		if (StoreDBOpen(client, user)) {
			Log(LOG_ERROR, "Couldn't access store database for %s", user);
			return -4;
		}
		if (StoreObjectDBCheckSchema(client, FALSE)) {
			Log(LOG_ERROR, "Couldn't check schema version of store database for %s", user);
			return -5;
		}
	}
	
	return 0;
}


/* closes the current store, if necessary */
void
UnselectStore(StoreClient *client)
{
	if (client->storeName && 
	client->stats.insertions + client->stats.updates + client->stats.deletions >
	(rand() % 50))
	{
		// FIXME: did optimise the Index here - could do a vacuum? could be costly tho...
	}

	if (client->watch.collection.guid > 0) {
		if (StoreWatcherRemove(client, &(client->watch.collection)))
			Log(LOG_ERROR, "Internal error removing client watch");
	}

	StoreDBClose(client);
	client->storedb = NULL;

	if (client->storeName) {
		MemFree(client->storeName);
		client->storeName = NULL;
	}
}

void
SetStoreReadonly(StoreClient *client, char const* reason)
{
	client->readonly = TRUE;
	client->ro_reason = reason;
}

void
UnsetStoreReadonly(StoreClient *client)
{
	client->readonly = FALSE;
	client->ro_reason = NULL;
}

void
UnselectUser(StoreClient *client)
{
    StoreToken *tok;
    StoreToken *next;

    for (tok = client->principal.tokens; 
         tok; 
         tok = next) 
    {
        next = tok->next;
        MemFree(tok);
    }
    client->principal.tokens = NULL;
    client->principal.type = STORE_PRINCIPAL_NONE;
    client->flags &= ~STORE_CLIENT_FLAG_IDENTITY;
}


static int
RemoveCurrentDirectory()
{
    DIR *dir;
    struct dirent *dirent;
    struct stat sb;
    int result = -1;

    dir = opendir(".");
    if (!dir) {
        return -1;
    }

    while ((dirent = readdir(dir))) {
        if (!strcmp(".", dirent->d_name) ||
            !strcmp("..", dirent->d_name))
        {
            continue;
        }

        if (stat(dirent->d_name, &sb)) {
            goto finish;
        }
        
        if (S_ISREG(sb.st_mode)) {
            unlink(dirent->d_name);
        } else if (S_ISDIR(sb.st_mode)) {
            if (chdir(dirent->d_name) ||
                RemoveCurrentDirectory() ||
                chdir(".."))
            {
                goto finish;
            }                             
        }
    }
    result = 0;

finish:
    closedir(dir);

    return result;
}


void
DeleteStore(StoreClient *client)
{
    char path[XPL_MAX_PATH];
    int cwd;

    strncpy(path, client->store, sizeof(path));
    cwd = open(".", O_RDONLY);
    if (cwd < 0) {
        return;
    }
    
    client->flags |= STORE_CLIENT_FLAG_DONT_CACHE;

    UnselectStore(client);
    
    if (chdir(path)) {
        return;
    }
    RemoveCurrentDirectory();
    fchdir(cwd);
}

BongoJsonResult
GetJson(StoreClient *client, StoreObject *object, BongoJsonNode **node, char *filepath)
{
    BongoJsonResult ret = BONGO_JSON_UNKNOWN_ERROR;
    char path[XPL_MAX_PATH + 1];
    FILE *fh = NULL;
    char *buf = NULL;
    
    if (filepath) {
        strncpy(path, filepath, XPL_MAX_PATH);
    } else {
        FindPathToDocument(client, object->collection_guid, object->guid, path, sizeof(path));
    }

    fh = fopen(path, "rb");
    if (!fh) {
        Log(LOG_ERROR, "Couldn't open file for doc " GUID_FMT, object->guid);
        goto finish;
    }

    /* FIXME: would be nice (and easy) to get a streaming API for
     * bongojson */ 
    buf = MemMalloc(object->size + 1);
    
    if (fread(buf, 1, object->size, fh) == object->size) {
        buf[object->size] = '\0';        
    } else {
        Log(LOG_ERROR, "Couldn't read doc " GUID_FMT, object->guid);
        goto finish;
    }
    
    ret = BongoJsonParseString(buf, node);

    if (ret != BONGO_JSON_OK) {
        Log(LOG_ERROR, "Couldn't parse json object %s", buf);
    }
    
finish :
    fclose (fh);
    if (buf) MemFree(buf);

    return ret;    
}
