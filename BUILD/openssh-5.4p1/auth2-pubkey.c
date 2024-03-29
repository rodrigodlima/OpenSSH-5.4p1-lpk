/* $OpenBSD: auth2-pubkey.c,v 1.21 2010/03/04 10:36:03 djm Exp $ */
/*
 * Copyright (c) 2000 Markus Friedl.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "includes.h"

#include <sys/types.h>
#include <sys/stat.h>

#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "xmalloc.h"
#include "ssh.h"
#include "ssh2.h"
#include "packet.h"
#include "buffer.h"
#include "log.h"
#include "servconf.h"
#include "compat.h"
#include "key.h"
#include "hostfile.h"
#include "auth.h"
#include "pathnames.h"
#include "uidswap.h"
#include "auth-options.h"
#include "canohost.h"
#ifdef GSSAPI
#include "ssh-gss.h"
#endif
#include "monitor_wrap.h"
#include "misc.h"
#include "authfile.h"

#ifdef WITH_LDAP_PUBKEY
#include "ldapauth.h"
#endif

/* import */
extern ServerOptions options;
extern u_char *session_id2;
extern u_int session_id2_len;

static int
userauth_pubkey(Authctxt *authctxt)
{
	Buffer b;
	Key *key = NULL;
	char *pkalg;
	u_char *pkblob, *sig;
	u_int alen, blen, slen;
	int have_sig, pktype;
	int authenticated = 0;

	if (!authctxt->valid) {
		debug2("userauth_pubkey: disabled because of invalid user");
		return 0;
	}
	have_sig = packet_get_char();
	if (datafellows & SSH_BUG_PKAUTH) {
		debug2("userauth_pubkey: SSH_BUG_PKAUTH");
		/* no explicit pkalg given */
		pkblob = packet_get_string(&blen);
		buffer_init(&b);
		buffer_append(&b, pkblob, blen);
		/* so we have to extract the pkalg from the pkblob */
		pkalg = buffer_get_string(&b, &alen);
		buffer_free(&b);
	} else {
		pkalg = packet_get_string(&alen);
		pkblob = packet_get_string(&blen);
	}
	pktype = key_type_from_name(pkalg);
	if (pktype == KEY_UNSPEC) {
		/* this is perfectly legal */
		logit("userauth_pubkey: unsupported public key algorithm: %s",
		    pkalg);
		goto done;
	}
	key = key_from_blob(pkblob, blen);
	if (key == NULL) {
		error("userauth_pubkey: cannot decode key: %s", pkalg);
		goto done;
	}
	if (key->type != pktype) {
		error("userauth_pubkey: type mismatch for decoded key "
		    "(received %d, expected %d)", key->type, pktype);
		goto done;
	}
	if (have_sig) {
		sig = packet_get_string(&slen);
		packet_check_eom();
		buffer_init(&b);
		if (datafellows & SSH_OLD_SESSIONID) {
			buffer_append(&b, session_id2, session_id2_len);
		} else {
			buffer_put_string(&b, session_id2, session_id2_len);
		}
		/* reconstruct packet */
		buffer_put_char(&b, SSH2_MSG_USERAUTH_REQUEST);
		buffer_put_cstring(&b, authctxt->user);
		buffer_put_cstring(&b,
		    datafellows & SSH_BUG_PKSERVICE ?
		    "ssh-userauth" :
		    authctxt->service);
		if (datafellows & SSH_BUG_PKAUTH) {
			buffer_put_char(&b, have_sig);
		} else {
			buffer_put_cstring(&b, "publickey");
			buffer_put_char(&b, have_sig);
			buffer_put_cstring(&b, pkalg);
		}
		buffer_put_string(&b, pkblob, blen);
#ifdef DEBUG_PK
		buffer_dump(&b);
#endif
		/* test for correct signature */
		authenticated = 0;
		if (PRIVSEP(user_key_allowed(authctxt->pw, key)) &&
		    PRIVSEP(key_verify(key, sig, slen, buffer_ptr(&b),
		    buffer_len(&b))) == 1)
			authenticated = 1;
		buffer_free(&b);
		xfree(sig);
	} else {
		debug("test whether pkalg/pkblob are acceptable");
		packet_check_eom();

		/* XXX fake reply and always send PK_OK ? */
		/*
		 * XXX this allows testing whether a user is allowed
		 * to login: if you happen to have a valid pubkey this
		 * message is sent. the message is NEVER sent at all
		 * if a user is not allowed to login. is this an
		 * issue? -markus
		 */
		if (PRIVSEP(user_key_allowed(authctxt->pw, key))) {
			packet_start(SSH2_MSG_USERAUTH_PK_OK);
			packet_put_string(pkalg, alen);
			packet_put_string(pkblob, blen);
			packet_send();
			packet_write_wait();
			authctxt->postponed = 1;
		}
	}
	if (authenticated != 1)
		auth_clear_options();
done:
	debug2("userauth_pubkey: authenticated %d pkalg %s", authenticated, pkalg);
	if (key != NULL)
		key_free(key);
	xfree(pkalg);
	xfree(pkblob);
	return authenticated;
}

/* return 1 if user allows given key */
static int
user_key_allowed2(struct passwd *pw, Key *key, char *file)
{
	char line[SSH_MAX_PUBKEY_BYTES];
	const char *reason;
	int found_key = 0;
	FILE *f;
	u_long linenum = 0;
	Key *found;
	char *fp;
#ifdef WITH_LDAP_PUBKEY
	ldap_key_t * k;
	unsigned int i = 0;
#endif

	/* Temporarily use the user's uid. */
	temporarily_use_uid(pw);

#ifdef WITH_LDAP_PUBKEY
	found_key = 0;
	/* allocate a new key type */
	found = key_new(key->type);
 
	/* first check if the options is enabled, then try.. */
	if (options.lpk.on) {
	    debug("[LDAP] trying LDAP first uid=%s",pw->pw_name);
	    if (ldap_ismember(&options.lpk, pw->pw_name) > 0) {
		if ((k = ldap_getuserkey(&options.lpk, pw->pw_name)) != NULL) {
		    /* Skip leading whitespace, empty and comment lines. */
		    for (i = 0 ; i < k->num ; i++) {
			/* dont forget if multiple keys to reset options */
			char *cp, *options = NULL;

			for (cp = (char *)k->keys[i]->bv_val; *cp == ' ' || *cp == '\t'; cp++)
			    ;
			if (!*cp || *cp == '\n' || *cp == '#')
			    continue;

			if (key_read(found, &cp) != 1) {
			    /* no key?  check if there are options for this key */
			    int quoted = 0;
			    debug2("[LDAP] user_key_allowed: check options: '%s'", cp);
			    options = cp;
			    for (; *cp && (quoted || (*cp != ' ' && *cp != '\t')); cp++) {
				if (*cp == '\\' && cp[1] == '"')
				    cp++;	/* Skip both */
				else if (*cp == '"')
				    quoted = !quoted;
			    }
			    /* Skip remaining whitespace. */
			    for (; *cp == ' ' || *cp == '\t'; cp++)
				;
			    if (key_read(found, &cp) != 1) {
				debug2("[LDAP] user_key_allowed: advance: '%s'", cp);
				/* still no key?  advance to next line*/
				continue;
			    }
			}

			if (key_equal(found, key) &&
				auth_parse_options(pw, options, file, linenum) == 1) {
			    found_key = 1;
			    debug("[LDAP] matching key found");
			    fp = key_fingerprint(found, SSH_FP_MD5, SSH_FP_HEX);
			    verbose("[LDAP] Found matching %s key: %s", key_type(found), fp);

			    /* restoring memory */
			    ldap_keys_free(k);
			    xfree(fp);
			    restore_uid();
			    key_free(found);
			    return found_key;
			    break;
			}
		    }/* end of LDAP for() */
		} else {
		    logit("[LDAP] no keys found for '%s'!", pw->pw_name);
		}
	    } else {
		logit("[LDAP] '%s' is not in '%s'", pw->pw_name, options.lpk.sgroup);
	    }
	}
#endif
	debug("trying public key file %s", file);
	f = auth_openkeyfile(file, pw, options.strict_modes);

	if (!f) {
		restore_uid();
		return 0;
	}

	found_key = 0;
	found = key_new(key_is_cert(key) ? KEY_UNSPEC : key->type);

	while (read_keyfile_line(f, file, line, sizeof(line), &linenum) != -1) {
		char *cp, *key_options = NULL;

		auth_clear_options();

		/* Skip leading whitespace, empty and comment lines. */
		for (cp = line; *cp == ' ' || *cp == '\t'; cp++)
			;
		if (!*cp || *cp == '\n' || *cp == '#')
			continue;

		if (key_read(found, &cp) != 1) {
			/* no key?  check if there are options for this key */
			int quoted = 0;
			debug2("user_key_allowed: check options: '%s'", cp);
			key_options = cp;
			for (; *cp && (quoted || (*cp != ' ' && *cp != '\t')); cp++) {
				if (*cp == '\\' && cp[1] == '"')
					cp++;	/* Skip both */
				else if (*cp == '"')
					quoted = !quoted;
			}
			/* Skip remaining whitespace. */
			for (; *cp == ' ' || *cp == '\t'; cp++)
				;
			if (key_read(found, &cp) != 1) {
				debug2("user_key_allowed: advance: '%s'", cp);
				/* still no key?  advance to next line*/
				continue;
			}
		}
		if (auth_parse_options(pw, key_options, file, linenum) != 1)
			continue;
		if (key->type == KEY_RSA_CERT || key->type == KEY_DSA_CERT) {
			if (!key_is_cert_authority)
				continue;
			if (!key_equal(found, key->cert->signature_key))
				continue;
			debug("matching CA found: file %s, line %lu",
			    file, linenum);
			fp = key_fingerprint(found, SSH_FP_MD5,
			    SSH_FP_HEX);
			verbose("Found matching %s CA: %s",
			    key_type(found), fp);
			xfree(fp);
			if (key_cert_check_authority(key, 0, 0, pw->pw_name,
			    &reason) != 0) {
				error("%s", reason);
				auth_debug_add("%s", reason);
				continue;
			}
			if (auth_cert_constraints(&key->cert->constraints,
			    pw) != 0)
				continue;
			found_key = 1;
			break;
		} else if (!key_is_cert_authority && key_equal(found, key)) {
			found_key = 1;
			debug("matching key found: file %s, line %lu",
			    file, linenum);
			fp = key_fingerprint(found, SSH_FP_MD5, SSH_FP_HEX);
			verbose("Found matching %s key: %s",
			    key_type(found), fp);
			xfree(fp);
			break;
		}
	}
	restore_uid();
	fclose(f);
	key_free(found);
	if (!found_key)
		debug2("key not found");
	return found_key;
}

/* Authenticate a certificate key against TrustedUserCAKeys */
static int
user_cert_trusted_ca(struct passwd *pw, Key *key)
{
	char *key_fp, *ca_fp;
	const char *reason;
	int ret = 0;

	if (!key_is_cert(key) || options.trusted_user_ca_keys == NULL)
		return 0;

	key_fp = key_fingerprint(key, SSH_FP_MD5, SSH_FP_HEX);
	ca_fp = key_fingerprint(key, SSH_FP_MD5, SSH_FP_HEX);

	if (key_in_file(key->cert->signature_key,
	    options.trusted_user_ca_keys, 1) != 1) {
		debug2("%s: CA %s %s is not listed in %s", __func__,
		    key_type(key->cert->signature_key), ca_fp,
		    options.trusted_user_ca_keys);
		goto out;
	}
	if (key_cert_check_authority(key, 0, 1, pw->pw_name, &reason) != 0) {
		error("%s", reason);
		auth_debug_add("%s", reason);
		goto out;
	}
	if (auth_cert_constraints(&key->cert->constraints, pw) != 0)
		goto out;

	verbose("%s certificate %s allowed by trusted %s key %s",
	    key_type(key), key_fp, key_type(key->cert->signature_key), ca_fp);
	ret = 1;

 out:
	if (key_fp != NULL)
		xfree(key_fp);
	if (ca_fp != NULL)
		xfree(ca_fp);
	return ret;
}

/* check whether given key is in .ssh/authorized_keys* */
int
user_key_allowed(struct passwd *pw, Key *key)
{
	int success;
	char *file;

	if (auth_key_is_revoked(key))
		return 0;
	if (key_is_cert(key) && auth_key_is_revoked(key->cert->signature_key))
		return 0;

	success = user_cert_trusted_ca(pw, key);
	if (success)
		return success;

	file = authorized_keys_file(pw);
	success = user_key_allowed2(pw, key, file);
	xfree(file);
	if (success)
		return success;

	/* try suffix "2" for backward compat, too */
	file = authorized_keys_file2(pw);
	success = user_key_allowed2(pw, key, file);
	xfree(file);
	return success;
}

Authmethod method_pubkey = {
	"publickey",
	userauth_pubkey,
	&options.pubkey_authentication
};
