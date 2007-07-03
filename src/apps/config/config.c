/* This program is free software, licensed under the terms of the GNU GPL.
 * See the Bongo COPYING file for full details
 * Copyright (c) 2007 Alex Hudson
 */

#include <xpl.h>
#include <stdio.h>
#include <nmlib.h>
#include <msgapi.h>
#include <bongo-buildinfo.h>
#include <bongostore.h>
#include <bongoagent.h>
#include <gnutls/gnutls.h>
#include <gnutls/x509.h>
#include <gcrypt.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "config.h"

BongoConfig config;

void
usage(void) {
	char *text = 
		"bongo-config - Configure parts of the Bongo system.\n\n"
		"Usage: bongo-config [options] [command]\n\n"
		"Options:\n"
		"  -v, --verbose                   verbose output\n"
		"  -s, --silent                    non-interactive mode\n\n"
		"Commands:\n"
		"  install                         do the initial Bongo install\n"
		"  crypto                          regenerate data needed for encryption\n"
                "  checkversion                    see if a newer Bongo is available\n" 
		"  tzcache                         regenerate cached timezone information\n"
		"";

	XplConsolePrintf("%s", text);
}

void
RunAsBongoUser() {
	if (XplSetRealUser(MsgGetUnprivilegedUser()) < 0) {    
		printf("bongo-config: Could not drop to unpriviliged user '%s'\n", MsgGetUnprivilegedUser());
		exit(-1);
	}
}

BOOL
NMAPSimpleCommand(StoreClient *client, char *command) {
	CCode ccode;
	
	NMAPSendCommandF(client->conn, command);
	ccode = NMAPReadAnswer(client->conn, client->buffer, sizeof(client->buffer), TRUE);
	if (1000 == ccode)
		return TRUE;

	return FALSE;
}

BOOL
SetAdminRights(StoreClient *client, char *document) {
	CCode ccode;
	char *rights = "grant user:admin all;";

	ccode = NMAPRunCommandF(client->conn, client->buffer, sizeof(client->buffer),
		"PROPSET %s nmap.access-control %d\r\n", document, strlen(rights));
	if (ccode == 2002) {
		NMAPSendCommandF(client->conn, rights);
		ccode = NMAPReadAnswer(client->conn, client->buffer, 
			sizeof(client->buffer), TRUE);
		if (ccode == 1000)
			return TRUE;
	} 
	return FALSE;
}

BOOL
PutOrReplaceConfig(StoreClient *client, char *collection, char *filename, char *content) {
	int len;
	CCode ccode;
	char command[1024];

	len = strlen(content);

	snprintf(command, 1000, "INFO %s/%s\r\n", collection, filename);
	ccode = NMAPSendCommandF(client->conn, command);
	ccode = NMAPReadAnswer(client->conn, client->buffer, sizeof(client->buffer), TRUE);
	while (ccode == 2001)
		ccode = NMAPReadAnswer(client->conn, client->buffer, sizeof(client->buffer), TRUE);

	if (ccode == 1000) {
		// INFO returned OK, document already exists
		snprintf(command, 1000, "REPLACE %s/%s %d\r\n", 
			collection, filename, len);
	} else {
		// INFO errored, so document doesn't exist
		snprintf(command, 1000, "WRITE %s %d %d \"F%s\"\r\n", 
			collection, STORE_DOCTYPE_CONFIG, len, filename);
	}

	NMAPSendCommandF(client->conn, command);
	ccode = NMAPReadAnswer(client->conn, client->buffer, sizeof(client->buffer), TRUE);

	if (ccode == 2002) {
		ConnWrite(client->conn, content, len);
		ConnFlush(client->conn);
		if (NMAPReadAnswer(client->conn, client->buffer, 
		  sizeof(client->buffer), TRUE) == 1000) {
			return TRUE;
		}
		XplConsolePrintf("ERROR: Couldn't write data\n");
	} else {
		XplConsolePrintf("ERROR: Wouldn't accept data\n");
	}
	return FALSE;
}

void
InitialStoreConfiguration() {
	char path[XPL_MAX_PATH];
	int store_pid;
	char *args[3];
	BOOL cal_success;
	
	// attempt to initialise cal lib. This creates cached data, and can be time
	// consuming: do it now, rather than in the store (for now, at least)
	XplConsolePrintf("Initializing calendar data...\n");
	cal_success = BongoCalInit(XPL_DEFAULT_DBF_DIR);
	if (! cal_success) {
		XplConsolePrintf("ERROR: Couldn't initialise calendar library\n");
		exit(1);
	}
	
	XplConsolePrintf("Initializing store...\n");
	store_pid = fork();
	if (store_pid == 0) { 	// child 
		static char *install = "--install";

		snprintf(path, XPL_MAX_PATH, "%s/%s", XPL_DEFAULT_BIN_DIR,
			"bongostore");
		args[0] = path;
		args[1] = install;
		args[2] = NULL;
		
		execv(path, args);
		XplConsolePrintf("ERROR: Couldn't start the store!\n");
		exit(1);
	} else {		// parent 
		StoreClient *client;
		int result = 0;

		XplDelay(1000); // FIXME: small hack, wait for store to start.
		
		client = malloc(sizeof(StoreClient));
		if (!client) {
			XplConsolePrintf("ERROR: Out of memory\n");
			goto storecleanup;
		}
		client->conn = NMAPConnect("127.0.0.1", NULL);
		if (!client->conn) {
			XplConsolePrintf("ERROR: Could not connect to store\n");
			goto storecleanup;
		}
		if (!NMAPAuthenticateToStore(client->conn, client->buffer, sizeof(client->buffer))) {
			XplConsolePrintf("ERROR: Could not authenticate to store\n");
			goto nmapcleanup;
		}
		
		if (!NMAPSimpleCommand(client, "STORE _system\r\n")) {
			XplConsolePrintf("ERROR: Couldn't access store\n");
			goto nmapcleanup;
		}
		result = NMAPSimpleCommand(client, "CREATE /config\r\n");
		if ((result==1000) || (result==4226)) {
			// collection can't be created and doesn't exist
			XplConsolePrintf("ERROR: Couldn't create collection\n");
			goto nmapcleanup;
		}
		if (! SetAdminRights(client, "/config")) {
			XplConsolePrintf("ERROR: Couldn't set acls on /config\n");
			goto nmapcleanup;
		}
		
		XplConsolePrintf("Setting default agent configuration...\n");
		if (!PutOrReplaceConfig(client, "/config", "manager", bongo_manager_config)) {
			XplConsolePrintf("ERROR: couldn't write /config/manager\n");
		}
		if (! SetAdminRights(client, "/config/manager")) {
			XplConsolePrintf("ERROR: Couldn't set acls on /config/manager\n");
			goto nmapcleanup;
		}
		if (!PutOrReplaceConfig(client, "/config", "antivirus", bongo_avirus_config)) {
			XplConsolePrintf("ERROR: couldn't write /config/antivirus\n");
		}
		if (! SetAdminRights(client, "/config/antivirus")) {
			XplConsolePrintf("ERROR: Couldn't set acls on /config/antivirus\n");
			goto nmapcleanup;
		}
		if (!PutOrReplaceConfig(client, "/config", "antispam", bongo_aspam_config)) {
			XplConsolePrintf("ERROR: couldn't write /config/antispam\n");
		}
		if (! SetAdminRights(client, "/config/antispam")) {
			XplConsolePrintf("ERROR: Couldn't set acls on /config/antispam\n");
			goto nmapcleanup;
		}

		XplConsolePrintf("Complete.\n");

nmapcleanup:
		NMAPQuit(client->conn);
		ConnFree(client->conn);
storecleanup:		
		if (kill(store_pid, SIGTERM) != 0) {
			XplConsolePrintf("ERROR: Couldn't shut down store\n");
		}
	}
}

void
GetInteractiveData(char *description, char **data, char *def) {
	int size = 128;
	char *line;

	if (*data != NULL)
		// don't ask for information we already have
		return;

	XplConsolePrintf("%s [%s]: ", description, def);
	*data = (char *)MemMalloc(sizeof(char)*size);
	line = fgets(*data, size, stdin);
	if (!line || *line == '\0') {
		MemFree(*data);
		*data = def;
	}
}

#define CERTSIZE	10240

BOOL
GenerateCryptoData() {
	gnutls_dh_params_t dh_params;
	gnutls_rsa_params_t rsa_params;
	gnutls_x509_privkey key;
	gnutls_x509_crt certificate;
	unsigned char dhdata[CERTSIZE], rsadata[CERTSIZE], private_key[CERTSIZE], public_cert[CERTSIZE];
	int ret;
	size_t dsize = sizeof(unsigned char) * (CERTSIZE);
	int cert_version = 1;
	mode_t private = S_IRUSR | S_IWUSR;
	FILE *params;
	static const char *org = "Bongo Project";
	static const char *unit = "Fake Certs Dept.";
	static const char *state = "None";
	static const char *country = "US";
	static const char *blank = "";
	
	gnutls_global_init();

	// hack: we need to ensure the files below can be saved...
	if (0 != mkdir(XPL_DEFAULT_DBF_DIR, 0755)) {
		XplConsolePrintf("Couldn't create data directory!\n");
	}
	
	GetInteractiveData("IP address to run on", &config.ip, "127.0.0.1");
	GetInteractiveData("DNS name to use:", &config.dns, "localhost");
	
	// save a random seed for faster Xpl startup in future.
	XplConsolePrintf("Creating random seed...\n");
	XplSaveRandomSeed();

	// various magic params
	XplConsolePrintf("Creating DH parameters...\n");
	gnutls_dh_params_init(&dh_params);
	gnutls_dh_params_generate2(dh_params, 1024);
	dsize = CERTSIZE;
	gnutls_dh_params_export_pkcs3 (dh_params, GNUTLS_X509_FMT_PEM, dhdata, &dsize);
	params = fopen(XPL_DEFAULT_DHPARAMS_PATH, "w");
	chmod(XPL_DEFAULT_DHPARAMS_PATH, private);
	if (params) {
		fprintf(params, "%s\n", dhdata);
		fclose(params);
	} else {
		XplConsolePrintf("ERROR: Couldn't write DH params to %s\n", XPL_DEFAULT_DHPARAMS_PATH);
		return FALSE;
	}

	XplConsolePrintf("Creating RSA parameters...\n");	
	gnutls_rsa_params_init(&rsa_params);
	gnutls_rsa_params_generate2(rsa_params, 512);
	dsize = CERTSIZE;
	gnutls_rsa_params_export_pkcs1 (rsa_params, GNUTLS_X509_FMT_PEM, rsadata, &dsize);
	params = fopen(XPL_DEFAULT_RSAPARAMS_PATH, "w");	
	if (params) {
		chmod(XPL_DEFAULT_RSAPARAMS_PATH, private);
		fprintf(params, "%s\n", dhdata);
		fclose(params);
	} else {
		XplConsolePrintf("ERROR: Couldn't write RSA params to %s\n", XPL_DEFAULT_DHPARAMS_PATH);
		return FALSE;
	}

	// create private key
	XplConsolePrintf("Creating private key...\n");
	ret = gnutls_x509_privkey_init(&key);
	if (ret < 0) {
		XplConsolePrintf("ERROR: %s\n", gnutls_strerror (ret));
		return FALSE;
	}
	ret = gnutls_x509_privkey_generate(key, GNUTLS_PK_RSA, 1024, 0);
	if (ret < 0) {
		XplConsolePrintf("ERROR: Couldn't create private key. %s\n", gnutls_strerror(ret));
		return FALSE;
	}
	dsize = CERTSIZE;
	ret = gnutls_x509_privkey_export(key, GNUTLS_X509_FMT_PEM, &private_key, &dsize);
	if (ret < 0) {
		XplConsolePrintf("ERROR: Couldn't export private key. %s\n", gnutls_strerror(ret));
		XplConsolePrintf("       Size: %d, Ret: %d\n", dsize, ret);
		return FALSE;
	} else {
		params = fopen(XPL_DEFAULT_KEY_PATH, "w");
		if (params) {
			chmod(XPL_DEFAULT_KEY_PATH, private);
			fprintf(params, "%s\n", private_key);
			fclose(params);
		} else {
			XplConsolePrintf("ERROR: Couldn't write private key to %s\n", XPL_DEFAULT_KEY_PATH);
			return FALSE;
		}
	}

	// create certificate
	XplConsolePrintf("Creating SSL/TLS certificate...\n");
	ret = gnutls_x509_crt_init(&certificate);
	if (ret < 0) {
		XplConsolePrintf("ERROR: %s\n", gnutls_strerror(ret));
		return FALSE;
	}
	gnutls_x509_crt_set_activation_time(certificate, time(NULL));
	gnutls_x509_crt_set_expiration_time(certificate, time(NULL) + (700 * 24 * 60 * 60));
	gnutls_x509_crt_set_key(certificate, key);
	gnutls_x509_crt_set_version(certificate, 1);
	gnutls_x509_crt_set_serial(certificate, &cert_version, sizeof(int));

	gnutls_x509_crt_set_dn_by_oid(certificate, 
		GNUTLS_OID_X520_ORGANIZATION_NAME, 0, org, strlen(org));
	gnutls_x509_crt_set_dn_by_oid(certificate, 
		GNUTLS_OID_X520_ORGANIZATIONAL_UNIT_NAME, 0, unit, strlen(unit));
	gnutls_x509_crt_set_dn_by_oid(certificate, 
		GNUTLS_OID_X520_STATE_OR_PROVINCE_NAME, 0, state, strlen(state));
	gnutls_x509_crt_set_dn_by_oid(certificate, 
		GNUTLS_OID_X520_COUNTRY_NAME, 0, country, strlen(country));
	gnutls_x509_crt_set_dn_by_oid(certificate,
		GNUTLS_OID_X520_LOCALITY_NAME, 0, blank, strlen(blank));
	gnutls_x509_crt_set_dn_by_oid(certificate, 
		GNUTLS_OID_X520_COMMON_NAME, 0, config.dns, strlen(config.dns));
	gnutls_x509_crt_set_dn_by_oid(certificate,
		GNUTLS_OID_LDAP_UID, 0, blank, strlen(blank));
	gnutls_x509_crt_set_subject_alternative_name (certificate,
		GNUTLS_SAN_DNSNAME, config.dns);
	
	gnutls_x509_crt_set_key_usage(certificate, GNUTLS_KEY_DIGITAL_SIGNATURE |
		GNUTLS_KEY_DATA_ENCIPHERMENT);

	ret = GNUTLS_SELF_SIGN(certificate, key);
	if (ret < 0) {
		XplConsolePrintf("ERROR: Couldn't self-sign certificate. %s\n", gnutls_strerror(ret));
		return FALSE;
	}

	dsize = CERTSIZE;
	ret = gnutls_x509_crt_export(certificate, GNUTLS_X509_FMT_PEM, &public_cert, &dsize);
	if (ret < 0) { 
		XplConsolePrintf("ERROR: Couldn't create certificate. %s\n", gnutls_strerror(ret));
		return FALSE;
	}
	params = fopen(XPL_DEFAULT_CERT_PATH, "w");
	if (params) {
		fprintf(params, "%s\n", public_cert);
		fclose(params);
	} else {
		XplConsolePrintf("ERROR: Couldn't write certificate to %s\n", XPL_DEFAULT_CERT_PATH);
		return FALSE;
	}
	
	gnutls_x509_crt_deinit(certificate);	
	gnutls_x509_privkey_deinit(key);
	
	MemFree(config.ip);
	MemFree(config.dns);

	return TRUE;
}

void
CheckVersion() {
        int current_version, available_version;
	BOOL custom_version;

	
	if (!MsgGetBuildVersion(&current_version, &custom_version)) {
		printf("Can't get information on current build!\n");
		return;
	} else {
		printf("Installed build: %s %s%d\n", BONGO_BUILD_BRANCH, BONGO_BUILD_VSTR, current_version);
		if (custom_version)
			printf("This is modified software.\n");
	}
	printf("Version currently available: ");
	if (!MsgGetAvailableVersion(&available_version)) {
		printf("unable to request online information.\n");
	} else {
		printf("%s%d\n", BONGO_BUILD_VSTR, available_version);
	}
}

void
TzCache() {
	char path[PATH_MAX];
	char *dbfdir = XPL_DEFAULT_DBF_DIR;

	snprintf(path, PATH_MAX, "%s/%s", dbfdir, "bongo-tz.cache");
	unlink(path);
    
	BongoCalInit(dbfdir);
}

int 
main(int argc, char *argv[]) {
	int next_arg = 0;
	int command = 0;
	int startup = 0;
	BongoAgent configtool;
	config.verbose = 0;

	if (!MemoryManagerOpen("bongo-config")) {
		XplConsolePrintf("ERROR: Failed to initialize memory manager\n");
		return 1;
	}
	
	// parse options
	while (++next_arg < argc && argv[next_arg][0] == '-') {
		char *arg = argv[next_arg];
		if (!strcmp(arg, "-v") || !strcmp(arg, "--verbose")) {
			config.verbose = TRUE;
		} else if (!strcmp(arg, "-s") || !strcmp(arg, "--silent")) {
			config.interactive = FALSE;
		} else if (!strcmp(arg, "--ip")) {
			next_arg++;
			config.ip = MemStrdup(argv[next_arg]);
		} else if (!strcmp(arg, "--domain")) {
			next_arg++;
			config.dns = MemStrdup(argv[next_arg]);
		} else {
			printf("Unrecognized option: %s\n", argv[next_arg]);
		}
	}

	// parse command 
	if (next_arg < argc) {
		if (!strcmp(argv[next_arg], "install")) { 
			command = 1;
		} else if (!strcmp(argv[next_arg], "crypto")) {
			command = 2;
		} else if (!strcmp(argv[next_arg], "checkversion")) {
			command = 3;
		} else if (!strcmp(argv[next_arg], "tzcache")) {
			command = 4;
		} else {
			printf("Unrecognized command: %s\n", argv[next_arg]);
		}
	} else {
		printf("ERROR: No command specified\n");
		usage();
	}

	// unusual libgcrypt options must occur before XplInit()
	if (command == 2) {
		gcry_control (GCRYCTL_ENABLE_QUICK_RANDOM, 0);
	}
	XplInit();
	
	// we don't want to setup libraries unless we need to (e.g., to run an agent)
	if (command == 1) {
		startup = BA_STARTUP_CONNIO;	
		if (-1 == BongoAgentInit(&configtool, "bongoconfig", "", DEFAULT_CONNECTION_TIMEOUT, startup)) {
			XplConsolePrintf("ERROR : Couldn't initialize Bongo libraries\n");
			return 2;
		}
	}

	// don't give away privileges if we're going to need them later.
	if (command != 1) {
		RunAsBongoUser();
	}

	switch(command) {
		case 1:
			InitialStoreConfiguration();
			break;
		case 2:
			GenerateCryptoData();
			break;
		case 3: 
			CheckVersion();
			break;
		case 4:
			TzCache();
			break;
		default:
			break;
	}

	MemoryManagerClose("bongo-config");
	exit(0);
}
