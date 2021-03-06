/*
 *   This program is is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or (at
 *   your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 * @file rlm_ldap.c
 * @brief LDAP authorization and authentication module.
 *
 * @author Arran Cudbard-Bell <a.cudbardb@freeradius.org>
 * @author Alan DeKok <aland@freeradius.org>
 *
 * @copyright 2012,2015 Arran Cudbard-Bell <a.cudbardb@freeradius.org>
 * @copyright 2013,2015 Network RADIUS SARL <info@networkradius.com>
 * @copyright 2012 Alan DeKok <aland@freeradius.org>
 * @copyright 1999-2013 The FreeRADIUS Server Project.
 */
RCSID("$Id$")

#include <freeradius-devel/rad_assert.h>

#include <stdarg.h>
#include <ctype.h>

#include "rlm_ldap.h"

#include <freeradius-devel/map_proc.h>

/*
 *	Scopes
 */
FR_NAME_NUMBER const ldap_scope[] = {
	{ "sub",	LDAP_SCOPE_SUB	},
	{ "one",	LDAP_SCOPE_ONE	},
	{ "base",	LDAP_SCOPE_BASE },
#ifdef LDAP_SCOPE_CHILDREN
	{ "children",	LDAP_SCOPE_CHILDREN },
#endif
	{  NULL , -1 }
};

#ifdef LDAP_OPT_X_TLS_NEVER
FR_NAME_NUMBER const ldap_tls_require_cert[] = {
	{ "never",	LDAP_OPT_X_TLS_NEVER	},
	{ "demand",	LDAP_OPT_X_TLS_DEMAND	},
	{ "allow",	LDAP_OPT_X_TLS_ALLOW	},
	{ "try",	LDAP_OPT_X_TLS_TRY	},
	{ "hard",	LDAP_OPT_X_TLS_HARD	},	/* oh yes, just like that */

	{  NULL , -1 }
};
#endif

static FR_NAME_NUMBER const ldap_dereference[] = {
	{ "never",	LDAP_DEREF_NEVER	},
	{ "searching",	LDAP_DEREF_SEARCHING	},
	{ "finding",	LDAP_DEREF_FINDING	},
	{ "always",	LDAP_DEREF_ALWAYS	},

	{  NULL , -1 }
};

static CONF_PARSER sasl_mech_dynamic[] = {
	{ FR_CONF_OFFSET("mech", PW_TYPE_TMPL | PW_TYPE_NOT_EMPTY, ldap_sasl_dynamic, mech) },
	{ FR_CONF_OFFSET("proxy", PW_TYPE_TMPL, ldap_sasl_dynamic, proxy) },
	{ FR_CONF_OFFSET("realm", PW_TYPE_TMPL, ldap_sasl_dynamic, realm) },
	CONF_PARSER_TERMINATOR
};

static CONF_PARSER sasl_mech_static[] = {
	{ FR_CONF_OFFSET("mech", PW_TYPE_STRING | PW_TYPE_NOT_EMPTY, ldap_sasl, mech) },
	{ FR_CONF_OFFSET("proxy", PW_TYPE_STRING, ldap_sasl, proxy) },
	{ FR_CONF_OFFSET("realm", PW_TYPE_STRING, ldap_sasl, realm) },
	CONF_PARSER_TERMINATOR
};

/*
 *	TLS Configuration
 */
static CONF_PARSER tls_config[] = {
	/*
	 *	Deprecated attributes
	 */
	{ FR_CONF_OFFSET("ca_file", PW_TYPE_FILE_INPUT, ldap_pool_inst_t, tls_ca_file) },

	{ FR_CONF_OFFSET("ca_path", PW_TYPE_FILE_INPUT, ldap_pool_inst_t, tls_ca_path) },

	{ FR_CONF_OFFSET("certificate_file", PW_TYPE_FILE_INPUT, ldap_pool_inst_t, tls_certificate_file) },

	{ FR_CONF_OFFSET("private_key_file", PW_TYPE_FILE_INPUT, ldap_pool_inst_t, tls_private_key_file) },

	/*
	 *	LDAP Specific TLS attributes
	 */
	{ FR_CONF_OFFSET("start_tls", PW_TYPE_BOOLEAN, ldap_pool_inst_t, start_tls), .dflt = "no" },

	{ FR_CONF_OFFSET("require_cert", PW_TYPE_STRING, ldap_pool_inst_t, tls_require_cert_str) },

	CONF_PARSER_TERMINATOR
};


static CONF_PARSER profile_config[] = {
	{ FR_CONF_OFFSET("filter", PW_TYPE_TMPL, rlm_ldap_t, profile_filter), .dflt = "(&)", .quote = T_SINGLE_QUOTED_STRING },	//!< Correct filter for when the DN is known.
	{ FR_CONF_OFFSET("attribute", PW_TYPE_STRING, rlm_ldap_t, profile_attr) },
	{ FR_CONF_OFFSET("default", PW_TYPE_TMPL, rlm_ldap_t, default_profile) },
	CONF_PARSER_TERMINATOR
};

/*
 *	User configuration
 */
static CONF_PARSER user_config[] = {
	{ FR_CONF_OFFSET("filter", PW_TYPE_TMPL, rlm_ldap_t, userobj_filter) },
	{ FR_CONF_OFFSET("scope", PW_TYPE_STRING, rlm_ldap_t, userobj_scope_str), .dflt = "sub" },
	{ FR_CONF_OFFSET("base_dn", PW_TYPE_TMPL, rlm_ldap_t, userobj_base_dn), .dflt = "", .quote = T_SINGLE_QUOTED_STRING },
	{ FR_CONF_OFFSET("sort_by", PW_TYPE_STRING, rlm_ldap_t, userobj_sort_by) },

	{ FR_CONF_OFFSET("access_attribute", PW_TYPE_STRING, rlm_ldap_t, userobj_access_attr) },
	{ FR_CONF_OFFSET("access_positive", PW_TYPE_BOOLEAN, rlm_ldap_t, access_positive), .dflt = "yes" },

	/* Should be deprecated */
	{ FR_CONF_OFFSET("sasl", PW_TYPE_SUBSECTION, rlm_ldap_t, user_sasl), .subcs = (void const *) sasl_mech_dynamic },
	CONF_PARSER_TERMINATOR
};

/*
 *	Group configuration
 */
static CONF_PARSER group_config[] = {
	{ FR_CONF_OFFSET("filter", PW_TYPE_STRING, rlm_ldap_t, groupobj_filter) },
	{ FR_CONF_OFFSET("scope", PW_TYPE_STRING, rlm_ldap_t, groupobj_scope_str), .dflt = "sub" },
	{ FR_CONF_OFFSET("base_dn", PW_TYPE_TMPL, rlm_ldap_t, groupobj_base_dn), .dflt = "", .quote = T_SINGLE_QUOTED_STRING },

	{ FR_CONF_OFFSET("name_attribute", PW_TYPE_STRING, rlm_ldap_t, groupobj_name_attr), .dflt = "cn" },
	{ FR_CONF_OFFSET("membership_attribute", PW_TYPE_STRING, rlm_ldap_t, userobj_membership_attr) },
	{ FR_CONF_OFFSET("membership_filter", PW_TYPE_STRING | PW_TYPE_XLAT, rlm_ldap_t, groupobj_membership_filter) },
	{ FR_CONF_OFFSET("cacheable_name", PW_TYPE_BOOLEAN, rlm_ldap_t, cacheable_group_name), .dflt = "no" },
	{ FR_CONF_OFFSET("cacheable_dn", PW_TYPE_BOOLEAN, rlm_ldap_t, cacheable_group_dn), .dflt = "no" },
	{ FR_CONF_OFFSET("cache_attribute", PW_TYPE_STRING, rlm_ldap_t, cache_attribute) },
	{ FR_CONF_OFFSET("group_attribute", PW_TYPE_STRING, rlm_ldap_t, group_attribute) },
	CONF_PARSER_TERMINATOR
};

static CONF_PARSER client_config[] = {
	{ FR_CONF_OFFSET("filter", PW_TYPE_STRING, rlm_ldap_t, clientobj_filter) },
	{ FR_CONF_OFFSET("scope", PW_TYPE_STRING, rlm_ldap_t, clientobj_scope_str), .dflt = "sub" },
	{ FR_CONF_OFFSET("base_dn", PW_TYPE_STRING, rlm_ldap_t, clientobj_base_dn), .dflt = "" },
	CONF_PARSER_TERMINATOR
};

/*
 *	Reference for accounting updates
 */
static const CONF_PARSER acct_section_config[] = {
	{ FR_CONF_OFFSET("reference", PW_TYPE_STRING | PW_TYPE_XLAT, ldap_acct_section_t, reference), .dflt = "." },
	CONF_PARSER_TERMINATOR
};

/*
 *	Various options that don't belong in the main configuration.
 *
 *	Note that these overlap a bit with the connection pool code!
 */
static CONF_PARSER option_config[] = {
	/*
	 *	Pool config items
	 */
	{ FR_CONF_OFFSET("chase_referrals", PW_TYPE_BOOLEAN, rlm_ldap_t, pool_inst.chase_referrals) },

	{ FR_CONF_OFFSET("use_referral_credentials", PW_TYPE_BOOLEAN, rlm_ldap_t, pool_inst.use_referral_credentials), .dflt = "no" },

	{ FR_CONF_OFFSET("rebind", PW_TYPE_BOOLEAN, rlm_ldap_t, pool_inst.rebind) },

#ifdef LDAP_CONTROL_X_SESSION_TRACKING
	{ FR_CONF_OFFSET("session_tracking", PW_TYPE_BOOLEAN, rlm_ldap_t, pool_inst.session_tracking), .dflt = "no" },
#endif

#ifdef LDAP_OPT_NETWORK_TIMEOUT
	/* timeout on network activity */
	{ FR_CONF_DEPRECATED("net_timeout", PW_TYPE_INTEGER, rlm_ldap_t, pool_inst.net_timeout), .dflt = "10" },
#endif

#ifdef LDAP_OPT_X_KEEPALIVE_IDLE
	{ FR_CONF_OFFSET("idle", PW_TYPE_INTEGER, rlm_ldap_t, pool_inst.keepalive_idle), .dflt = "60" },
#endif
#ifdef LDAP_OPT_X_KEEPALIVE_PROBES
	{ FR_CONF_OFFSET("probes", PW_TYPE_INTEGER, rlm_ldap_t, pool_inst.keepalive_probes), .dflt = "3" },
#endif
#ifdef LDAP_OPT_X_KEEPALIVE_INTERVAL
	{ FR_CONF_OFFSET("interval", PW_TYPE_INTEGER, rlm_ldap_t, pool_inst.keepalive_interval), .dflt = "30" },
#endif

	{ FR_CONF_OFFSET("dereference", PW_TYPE_STRING, rlm_ldap_t, pool_inst.dereference_str) },

	/* allow server unlimited time for search (server-side limit) */
	{ FR_CONF_OFFSET("srv_timelimit", PW_TYPE_INTEGER, rlm_ldap_t, pool_inst.srv_timelimit), .dflt = "20" },

	/*
	 *	Instance config items
	 */
	/* timeout for search results */
	{ FR_CONF_OFFSET("res_timeout", PW_TYPE_INTEGER, rlm_ldap_t, res_timeout), .dflt = "20" },

	CONF_PARSER_TERMINATOR
};

static const CONF_PARSER global_config[] = {
	{ FR_CONF_OFFSET("random_file", PW_TYPE_FILE_EXISTS, rlm_ldap_t, tls_random_file) },

	{ FR_CONF_OFFSET("ldap_debug", PW_TYPE_INTEGER, rlm_ldap_t, ldap_debug), .dflt = "0x0000" },		/* Debugging flags to the server */

	CONF_PARSER_TERMINATOR
};

static const CONF_PARSER module_config[] = {
	/*
	 *	Pool config items
	 */
	{ FR_CONF_OFFSET("server", PW_TYPE_STRING | PW_TYPE_MULTI, rlm_ldap_t, pool_inst.server_str) },	/* Do not set to required */

	{ FR_CONF_OFFSET("port", PW_TYPE_SHORT, rlm_ldap_t, pool_inst.port) },

	{ FR_CONF_OFFSET("identity", PW_TYPE_STRING, rlm_ldap_t, pool_inst.admin_identity) },
	{ FR_CONF_OFFSET("password", PW_TYPE_STRING | PW_TYPE_SECRET, rlm_ldap_t, pool_inst.admin_password) },

	{ FR_CONF_OFFSET("sasl", PW_TYPE_SUBSECTION, rlm_ldap_t, pool_inst.admin_sasl), .subcs = (void const *) sasl_mech_static },

	{ FR_CONF_OFFSET("valuepair_attribute", PW_TYPE_STRING, rlm_ldap_t, valuepair_attr) },

#ifdef WITH_EDIR
	/* support for eDirectory Universal Password */
	{ FR_CONF_OFFSET("edir", PW_TYPE_BOOLEAN, rlm_ldap_t, edir) }, /* NULL defaults to "no" */

	/*
	 *	Attempt to bind with the cleartext password we got from eDirectory
	 *	Universal password for additional authorization checks.
	 */
	{ FR_CONF_OFFSET("edir_autz", PW_TYPE_BOOLEAN, rlm_ldap_t, edir_autz) }, /* NULL defaults to "no" */
#endif

	{ FR_CONF_OFFSET("read_clients", PW_TYPE_BOOLEAN, rlm_ldap_t, do_clients) }, /* NULL defaults to "no" */

	{ FR_CONF_POINTER("user", PW_TYPE_SUBSECTION, NULL), .subcs = (void const *) user_config },

	{ FR_CONF_POINTER("group", PW_TYPE_SUBSECTION, NULL), .subcs = (void const *) group_config },

	{ FR_CONF_POINTER("client", PW_TYPE_SUBSECTION, NULL), .subcs = (void const *) client_config },

	{ FR_CONF_POINTER("profile", PW_TYPE_SUBSECTION, NULL), .subcs = (void const *) profile_config },

	{ FR_CONF_POINTER("options", PW_TYPE_SUBSECTION, NULL), .subcs = (void const *) option_config },

	{ FR_CONF_POINTER("global", PW_TYPE_SUBSECTION, NULL), .subcs = (void const *) global_config },

	{ FR_CONF_OFFSET("tls", PW_TYPE_SUBSECTION, rlm_ldap_t, pool_inst), .subcs = (void const *) tls_config },
	CONF_PARSER_TERMINATOR
};


static LDAP *global_handle;			//!< Hack for OpenLDAP libldap global initialisation.

static ssize_t ldap_escape_xlat(UNUSED TALLOC_CTX *ctx, char **out, size_t outlen,
			 	UNUSED void const *mod_inst, UNUSED void const *xlat_inst,
			 	REQUEST *request, char const *fmt)
{
	return rlm_ldap_escape_func(request, *out, outlen, fmt, NULL);
}

static ssize_t ldap_unescape_xlat(UNUSED TALLOC_CTX *ctx, char **out, size_t outlen,
				  UNUSED void const *mod_inst, UNUSED void const *xlat_inst,
			 	  REQUEST *request, char const *fmt)
{
	return rlm_ldap_unescape_func(request, *out, outlen, fmt, NULL);
}

/** Parse a subset (just server side sort for now) of LDAP URL extensions
 *
 * @param[out] sss		Where to write a pointer to the server side sort control
 *				we created.
 * @param[in] request		The current request.
 * @param[in] conn		Handle to allocate controls under.
 * @param[in] extensions	A NULL terminated array of extensions.
 * @return
 *	- 0 on success.
 *	- -1 on failure.
 */
static int ldap_parse_url_extensions(LDAPControl **sss, REQUEST *request, ldap_handle_t *conn, char **extensions)
{
	int i;

	*sss = NULL;

	if (!extensions) return 0;

	/*
	 *	Parse extensions in the LDAP URL
	 */
	for (i = 0; extensions[i]; i++) {
		char *p;
		bool is_critical = false;

		p = extensions[i];
		if (*p == '!') {
			is_critical = true;
			p++;
		}

#ifdef HAVE_LDAP_CREATE_SORT_CONTROL
		/*
		 *	Server side sort control
		 */
		if (strncmp(p, "sss", 3) == 0) {
			LDAPSortKey	**keys;
			int		ret;

			p += 3;
			p = strchr(p, '=');
			if (!p) {
				REDEBUG("Server side sort extension must be in the format \"[!]sss=<key>[,key]\"");
				return -1;
			}
			p++;

			ret = ldap_create_sort_keylist(&keys, p);
			if (ret != LDAP_SUCCESS) {
				REDEBUG("Invalid server side sort value \"%s\": %s", p, ldap_err2string(ret));
				return -1;
			}

			if (*sss) ldap_control_free(*sss);

			ret = ldap_create_sort_control(conn->handle, keys, is_critical ? 1 : 0, sss);
			ldap_free_sort_keylist(keys);
			if (ret != LDAP_SUCCESS) {
				ERROR("Failed creating server sort control: %s", ldap_err2string(ret));
				return -1;
			}

			continue;
		}
#endif

		RWDEBUG("URL extension \"%s\" ignored", p);
	}

	return 0;
}

/** Expand an LDAP URL into a query, and return a string result from that query.
 *
 */
static ssize_t ldap_xlat(UNUSED TALLOC_CTX *ctx, char **out, size_t outlen,
			 void const *mod_inst, UNUSED void const *xlat_inst,
			 REQUEST *request, char const *fmt)
{
	ldap_rcode_t		status;
	size_t			len = 0;
	rlm_ldap_t const	*inst = mod_inst;

	LDAPURLDesc		*ldap_url;
	LDAPMessage		*result = NULL;
	LDAPMessage		*entry = NULL;

	struct berval		**values;

	ldap_handle_t		*conn;
	int			ldap_errno;

	char const		*url;
	char const		**attrs;

	LDAPControl		*server_ctrls[] = { NULL, NULL };

	url = fmt;

	if (!ldap_is_ldap_url(url)) {
		REDEBUG("String passed does not look like an LDAP URL");
		return -1;
	}

	if (ldap_url_parse(url, &ldap_url)){
		REDEBUG("Parsing LDAP URL failed");
		return -1;
	}

	/*
	 *	Nothing, empty string, "*" string, or got 2 things, die.
	 */
	if (!ldap_url->lud_attrs || !ldap_url->lud_attrs[0] ||
	    !*ldap_url->lud_attrs[0] ||
	    (strcmp(ldap_url->lud_attrs[0], "*") == 0) ||
	    ldap_url->lud_attrs[1]) {
		REDEBUG("Bad attributes list in LDAP URL. URL must specify exactly one attribute to retrieve");

		goto free_urldesc;
	}

	conn = mod_conn_get(inst, request);
	if (!conn) goto free_urldesc;

	memcpy(&attrs, &ldap_url->lud_attrs, sizeof(attrs));

	if (ldap_parse_url_extensions(&server_ctrls[0], request, conn, ldap_url->lud_exts) < 0) goto free_socket;

	status = rlm_ldap_search(&result, inst, request, &conn, ldap_url->lud_dn, ldap_url->lud_scope,
				 ldap_url->lud_filter, attrs, server_ctrls, NULL);

#ifdef HAVE_LDAP_CREATE_SORT_CONTROL
	if (server_ctrls[0]) ldap_control_free(server_ctrls[0]);
#endif

	switch (status) {
	case LDAP_PROC_SUCCESS:
		break;

	default:
		goto free_socket;
	}

	rad_assert(conn);
	rad_assert(result);

	entry = ldap_first_entry(conn->handle, result);
	if (!entry) {
		ldap_get_option(conn->handle, LDAP_OPT_RESULT_CODE, &ldap_errno);
		REDEBUG("Failed retrieving entry: %s", ldap_err2string(ldap_errno));
		len = -1;
		goto free_result;
	}

	values = ldap_get_values_len(conn->handle, entry, ldap_url->lud_attrs[0]);
	if (!values) {
		RDEBUG("No \"%s\" attributes found in specified object", ldap_url->lud_attrs[0]);
		goto free_result;
	}

	if (values[0]->bv_len >= outlen) goto free_values;

	memcpy(*out, values[0]->bv_val, values[0]->bv_len + 1);	/* +1 as strlcpy expects buffer size */
	len = values[0]->bv_len;

free_values:
	ldap_value_free_len(values);
free_result:
	ldap_msgfree(result);
free_socket:
	mod_conn_release(inst, request, conn);
free_urldesc:
	ldap_free_urldesc(ldap_url);

	return len;
}

/*
 *	Verify the result of the map.
 */
static int ldap_map_verify(CONF_SECTION *cs, UNUSED void *mod_inst, UNUSED void *proc_inst,
			   vp_tmpl_t const *src, UNUSED vp_map_t const *maps)
{
	if (!src) {
		cf_log_err_cs(cs, "Missing LDAP URI");

		return -1;
	}

	return 0;
}

/** Perform a search and map the result of the search to server attributes
 *
 * Unlike LDAP xlat, this can be used to process attributes from multiple entries.
 *
 * @todo For xlat expansions we need to parse the raw URL first, and then apply
 *	different escape functions to the different parts.
 *
 * @param[in] mod_inst #rlm_ldap_t
 * @param[in] proc_inst unused.
 * @param[in,out] request The current request.
 * @param[in] url LDAP url specifying base DN and filter.
 * @param[in] maps Head of the map list.
 * @return
 *	- #RLM_MODULE_NOOP no rows were returned.
 *	- #RLM_MODULE_UPDATED if one or more #VALUE_PAIR were added to the #REQUEST.
 *	- #RLM_MODULE_FAIL if an error occurred.
 */
static rlm_rcode_t mod_map_proc(void *mod_inst, UNUSED void *proc_inst, REQUEST *request,
				vp_tmpl_t const *url, vp_map_t const *maps)
{
	rlm_rcode_t		rcode = RLM_MODULE_UPDATED;
	rlm_ldap_t		*inst = talloc_get_type_abort(mod_inst, rlm_ldap_t);
	ldap_rcode_t		status;

	LDAPURLDesc		*ldap_url;

	LDAPMessage		*result = NULL;
	LDAPMessage		*entry = NULL;
	vp_map_t const		*map;
	char			*url_str;

	ldap_handle_t		*conn;

	LDAPControl		*server_ctrls[] = { NULL, NULL };

	rlm_ldap_map_exp_t	expanded; /* faster than allocing every time */

	if (tmpl_aexpand(request, &url_str, request, url, rlm_ldap_escape_func, NULL) < 0) {
		return RLM_MODULE_FAIL;
	}

	if (!ldap_is_ldap_url(url_str)) {
		REDEBUG("Map query string does not look like a valid LDAP URI");
		goto free_urlstr;
	}

	if (ldap_url_parse(url_str, &ldap_url)){
		REDEBUG("Parsing LDAP URL failed");
		goto free_urlstr;
	}

	/*
	 *	Expand the RHS of the maps to get the name of the attributes.
	 */
	if (rlm_ldap_map_expand(&expanded, request, maps) < 0) {
		rcode = RLM_MODULE_FAIL;
		goto free_urldesc;
	}

	conn = mod_conn_get(inst, request);
	if (!conn) goto free_expanded;

	if (ldap_parse_url_extensions(&server_ctrls[0], request, conn, ldap_url->lud_exts) < 0) goto free_socket;

	status = rlm_ldap_search(&result, inst, request, &conn, ldap_url->lud_dn, ldap_url->lud_scope,
				 ldap_url->lud_filter, expanded.attrs, server_ctrls, NULL);

#ifdef HAVE_LDAP_CREATE_SORT_CONTROL
	if (server_ctrls[0]) ldap_control_free(server_ctrls[0]);
#endif

	switch (status) {
	case LDAP_PROC_SUCCESS:
		break;

	case LDAP_PROC_NO_RESULT:
		rcode = RLM_MODULE_NOOP;
		goto free_socket;

	default:
		rcode = RLM_MODULE_FAIL;
		goto free_socket;
	}

	rad_assert(conn);
	rad_assert(result);

	for (entry = ldap_first_entry(conn->handle, result);
	     entry;
	     entry = ldap_next_entry(conn->handle, entry)) {
		char	*dn = NULL;
		int	i;


		if (RDEBUG_ENABLED2) {
			dn = ldap_get_dn(conn->handle, entry);
			RDEBUG2("Processing \"%s\"", dn);
		}

		RINDENT();
		for (map = maps, i = 0;
		     map != NULL;
		     map = map->next, i++) {
			int			ret;
			rlm_ldap_result_t	attr;

			attr.values = ldap_get_values_len(conn->handle, entry, expanded.attrs[i]);
			if (!attr.values) {
				/*
				 *	Many LDAP directories don't expose the DN of
				 *	the object as an attribute, so we need this
				 *	hack, to allow the user to retrieve it.
				 */
				if (strcmp(LDAP_VIRTUAL_DN_ATTR, expanded.attrs[i]) == 0) {
					struct berval value;
					struct berval *values[2] = { &value, NULL };

					if (!dn) dn = ldap_get_dn(conn->handle, entry);
					value.bv_val = dn;
					value.bv_len = strlen(dn);

					attr.values = values;
					attr.count = 1;

					ret = map_to_request(request, map, rlm_ldap_map_getvalue, &attr);
					if (ret == -1) {
						rcode = RLM_MODULE_FAIL;
						ldap_memfree(dn);
						goto free_result;
					}
					continue;
				}

				RDEBUG3("Attribute \"%s\" not found in LDAP object", expanded.attrs[i]);

				continue;
			}
			attr.count = ldap_count_values_len(attr.values);

			ret = map_to_request(request, map, rlm_ldap_map_getvalue, &attr);
			ldap_value_free_len(attr.values);
			if (ret == -1) {
				rcode = RLM_MODULE_FAIL;
				ldap_memfree(dn);
				goto free_result;
			}
		}
		ldap_memfree(dn);
		REXDENT();
	}

free_result:
	ldap_msgfree(result);
free_socket:
	mod_conn_release(inst, request, conn);
free_expanded:
	talloc_free(expanded.ctx);
free_urldesc:
	ldap_free_urldesc(ldap_url);
free_urlstr:
	talloc_free(url_str);

	return rcode;
}

/** Perform LDAP-Group comparison checking
 *
 * Attempts to match users to groups using a variety of methods.
 *
 * @param instance of the rlm_ldap module.
 * @param request Current request.
 * @param thing Unknown.
 * @param check Which group to check for user membership.
 * @param check_pairs Unknown.
 * @param reply_pairs Unknown.
 * @return
 *	- 1 on failure (or if the user is not a member).
 *	- 0 on success.
 */
static int rlm_ldap_groupcmp(void *instance, REQUEST *request, UNUSED VALUE_PAIR *thing, VALUE_PAIR *check,
			     UNUSED VALUE_PAIR *check_pairs, UNUSED VALUE_PAIR **reply_pairs)
{
	rlm_ldap_t const	*inst = instance;
	rlm_rcode_t		rcode;

	bool			found = false;
	bool			check_is_dn;

	ldap_handle_t		*conn = NULL;
	char const		*user_dn;

	rad_assert(inst->groupobj_base_dn);

	RDEBUG("Searching for user in group \"%s\"", check->vp_strvalue);

	if (check->vp_length == 0) {
		REDEBUG("Cannot do comparison (group name is empty)");
		return 1;
	}

	/*
	 *	Check if we can do cached membership verification
	 */
	check_is_dn = rlm_ldap_is_dn(check->vp_strvalue, check->vp_length);
	if (check_is_dn) {
		char *norm;

		MEM(norm = talloc_memdup(check, check->vp_strvalue, talloc_array_length(check->vp_strvalue)));
		rlm_ldap_normalise_dn(norm, check->vp_strvalue);
		fr_pair_value_strsteal(check, norm);
	}
	if ((check_is_dn && inst->cacheable_group_dn) || (!check_is_dn && inst->cacheable_group_name)) {
		switch (rlm_ldap_check_cached(inst, request, check)) {
		case RLM_MODULE_NOTFOUND:
			found = false;
			goto finish;

		case RLM_MODULE_OK:
			found = true;
			goto finish;
		/*
		 *	Fallback to dynamic search on failure
		 */
		case RLM_MODULE_FAIL:
		case RLM_MODULE_INVALID:
		default:
			break;
		}
	}

	conn = mod_conn_get(inst, request);
	if (!conn) return 1;

	/*
	 *	This is used in the default membership filter.
	 */
	user_dn = rlm_ldap_find_user(inst, request, &conn, NULL, false, NULL, &rcode);
	if (!user_dn) {
		mod_conn_release(inst, request, conn);
		return 1;
	}

	rad_assert(conn);

	/*
	 *	Check groupobj user membership
	 */
	if (inst->groupobj_membership_filter) {
		switch (rlm_ldap_check_groupobj_dynamic(inst, request, &conn, check)) {
		case RLM_MODULE_NOTFOUND:
			break;

		case RLM_MODULE_OK:
			found = true;

		default:
			goto finish;
		}
	}

	rad_assert(conn);

	/*
	 *	Check userobj group membership
	 */
	if (inst->userobj_membership_attr) {
		switch (rlm_ldap_check_userobj_dynamic(inst, request, &conn, user_dn, check)) {
		case RLM_MODULE_NOTFOUND:
			break;

		case RLM_MODULE_OK:
			found = true;

		default:
			goto finish;
		}
	}

	rad_assert(conn);

finish:
	if (conn) mod_conn_release(inst, request, conn);

	if (!found) {
		RDEBUG("User is not a member of \"%s\"", check->vp_strvalue);

		return 1;
	}

	return 0;
}

static rlm_rcode_t mod_authenticate(void *instance, UNUSED void *thread, REQUEST *request) CC_HINT(nonnull);
static rlm_rcode_t CC_HINT(nonnull) mod_authenticate(void *instance, UNUSED void *thread, REQUEST *request)
{
	rlm_rcode_t		rcode;
	ldap_rcode_t		status;
	char const		*dn;
	rlm_ldap_t const	*inst = instance;
	ldap_handle_t		*conn;

	char			sasl_mech_buff[LDAP_MAX_DN_STR_LEN];
	char			sasl_proxy_buff[LDAP_MAX_DN_STR_LEN];
	char			sasl_realm_buff[LDAP_MAX_DN_STR_LEN];
	ldap_sasl		sasl;

	/*
	 * Ensure that we're being passed a plain-text password, and not
	 * anything else.
	 */

	if (!request->username) {
		REDEBUG("Attribute \"User-Name\" is required for authentication");

		return RLM_MODULE_INVALID;
	}

	if (!request->password ||
	    (request->password->da->attr != PW_USER_PASSWORD)) {
		RWDEBUG("You have set \"Auth-Type := LDAP\" somewhere");
		RWDEBUG("*********************************************");
		RWDEBUG("* THAT CONFIGURATION IS WRONG.  DELETE IT.   ");
		RWDEBUG("* YOU ARE PREVENTING THE SERVER FROM WORKING");
		RWDEBUG("*********************************************");

		REDEBUG("Attribute \"User-Password\" is required for authentication");

		return RLM_MODULE_INVALID;
	}

	if (request->password->vp_length == 0) {
		REDEBUG("Empty password supplied");

		return RLM_MODULE_INVALID;
	}

	conn = mod_conn_get(inst, request);
	if (!conn) return RLM_MODULE_FAIL;

	/*
	 *	Expand dynamic SASL fields
	 */
	if (inst->user_sasl.mech) {
		memset(&sasl, 0, sizeof(sasl));

		if (tmpl_expand(&sasl.mech, sasl_mech_buff, sizeof(sasl_mech_buff), request,
				inst->user_sasl.mech, rlm_ldap_escape_func, inst) < 0) {
			REDEBUG("Failed expanding user.sasl.mech: %s", fr_strerror());
			rcode = RLM_MODULE_FAIL;
			goto finish;
		}

		if (inst->user_sasl.proxy) {
			if (tmpl_expand(&sasl.proxy, sasl_proxy_buff, sizeof(sasl_proxy_buff), request,
					inst->user_sasl.proxy, rlm_ldap_escape_func, inst) < 0) {
				REDEBUG("Failed expanding user.sasl.proxy: %s", fr_strerror());
				rcode = RLM_MODULE_FAIL;
				goto finish;
			}
		}

		if (inst->user_sasl.realm) {
			if (tmpl_expand(&sasl.realm, sasl_realm_buff, sizeof(sasl_realm_buff), request,
					inst->user_sasl.realm, rlm_ldap_escape_func, inst) < 0) {
				REDEBUG("Failed expanding user.sasl.realm: %s", fr_strerror());
				rcode = RLM_MODULE_FAIL;
				goto finish;
			}
		}
	}

	RDEBUG("Login attempt by \"%s\"", request->username->vp_strvalue);

	/*
	 *	Get the DN by doing a search.
	 */
	dn = rlm_ldap_find_user(inst, request, &conn, NULL, false, NULL, &rcode);
	if (!dn) {
		mod_conn_release(inst, request, conn);

		return rcode;
	}
	conn->rebound = true;
	status = rlm_ldap_bind(inst, request, &conn, dn, request->password->vp_strvalue,
			       inst->user_sasl.mech ? &sasl : NULL, true, NULL, NULL, NULL);
	switch (status) {
	case LDAP_PROC_SUCCESS:
		rcode = RLM_MODULE_OK;
		RDEBUG("Bind as user \"%s\" was successful", dn);
		break;

	case LDAP_PROC_NOT_PERMITTED:
		rcode = RLM_MODULE_USERLOCK;
		break;

	case LDAP_PROC_REJECT:
		rcode = RLM_MODULE_REJECT;
		break;

	case LDAP_PROC_BAD_DN:
		rcode = RLM_MODULE_INVALID;
		break;

	case LDAP_PROC_NO_RESULT:
		rcode = RLM_MODULE_NOTFOUND;
		break;

	default:
		rcode = RLM_MODULE_FAIL;
		break;
	};

finish:
	mod_conn_release(inst, request, conn);

	return rcode;
}

/** Search for and apply an LDAP profile
 *
 * LDAP profiles are mapped using the same attribute map as user objects, they're used to add common
 * sets of attributes to the request.
 *
 * @param[in] inst rlm_ldap configuration.
 * @param[in] request Current request.
 * @param[in,out] pconn to use. May change as this function calls functions which auto re-connect.
 * @param[in] dn of profile object to apply.
 * @param[in] expanded Structure containing a list of xlat expanded attribute names and mapping
information.
 * @return One of the RLM_MODULE_* values.
 */
static rlm_rcode_t rlm_ldap_map_profile(rlm_ldap_t const *inst, REQUEST *request, ldap_handle_t **pconn,
					char const *dn, rlm_ldap_map_exp_t const *expanded)
{
	rlm_rcode_t	rcode = RLM_MODULE_OK;
	ldap_rcode_t	status;
	LDAPMessage	*result = NULL, *entry = NULL;
	int		ldap_errno;
	LDAP		*handle = (*pconn)->handle;
	char const	*filter;
	char		filter_buff[LDAP_MAX_FILTER_STR_LEN];

	rad_assert(inst->profile_filter); 	/* We always have a default filter set */

	if (!dn || !*dn) return RLM_MODULE_OK;

	if (tmpl_expand(&filter, filter_buff, sizeof(filter_buff), request,
			inst->profile_filter, rlm_ldap_escape_func, NULL) < 0) {
		REDEBUG("Failed creating profile filter");

		return RLM_MODULE_INVALID;
	}

	status = rlm_ldap_search(&result, inst, request, pconn, dn,
				 LDAP_SCOPE_BASE, filter, expanded->attrs, NULL, NULL);
	switch (status) {
	case LDAP_PROC_SUCCESS:
		break;

	case LDAP_PROC_BAD_DN:
	case LDAP_PROC_NO_RESULT:
		RDEBUG("Profile object \"%s\" not found", dn);
		return RLM_MODULE_NOTFOUND;

	default:
		return RLM_MODULE_FAIL;
	}

	rad_assert(*pconn);
	rad_assert(result);

	entry = ldap_first_entry(handle, result);
	if (!entry) {
		ldap_get_option(handle, LDAP_OPT_RESULT_CODE, &ldap_errno);
		REDEBUG("Failed retrieving entry: %s", ldap_err2string(ldap_errno));

		rcode = RLM_MODULE_NOTFOUND;

		goto free_result;
	}

	RDEBUG("Processing profile attributes");
	RINDENT();
	if (rlm_ldap_map_do(inst, request, handle, expanded, entry) > 0) rcode = RLM_MODULE_UPDATED;
	REXDENT();

free_result:
	ldap_msgfree(result);

	return rcode;
}

static rlm_rcode_t mod_authorize(void *instance, UNUSED void *thread, REQUEST *request) CC_HINT(nonnull);
static rlm_rcode_t mod_authorize(void *instance, UNUSED void *thread, REQUEST *request)
{
	rlm_rcode_t		rcode = RLM_MODULE_OK;
	ldap_rcode_t		status;
	int			ldap_errno;
	int			i;
	rlm_ldap_t const	*inst = instance;
	struct berval		**values;
	VALUE_PAIR		*vp;
	ldap_handle_t		*conn;
	LDAPMessage		*result, *entry;
	char const 		*dn = NULL;
	rlm_ldap_map_exp_t	expanded; /* faster than allocing every time */

	/*
	 *	Don't be tempted to add a check for request->username
	 *	or request->password here. rlm_ldap.authorize can be used for
	 *	many things besides searching for users.
	 */

	if (rlm_ldap_map_expand(&expanded, request, inst->user_map) < 0) return RLM_MODULE_FAIL;

	conn = mod_conn_get(inst, request);
	if (!conn) return RLM_MODULE_FAIL;

	/*
	 *	Add any additional attributes we need for checking access, memberships, and profiles
	 */
	if (inst->userobj_access_attr) {
		expanded.attrs[expanded.count++] = inst->userobj_access_attr;
	}

	if (inst->userobj_membership_attr && (inst->cacheable_group_dn || inst->cacheable_group_name)) {
		expanded.attrs[expanded.count++] = inst->userobj_membership_attr;
	}

	if (inst->profile_attr) {
		expanded.attrs[expanded.count++] = inst->profile_attr;
	}

	if (inst->valuepair_attr) {
		expanded.attrs[expanded.count++] = inst->valuepair_attr;
	}

	expanded.attrs[expanded.count] = NULL;

	dn = rlm_ldap_find_user(inst, request, &conn, expanded.attrs, true, &result, &rcode);
	if (!dn) {
		goto finish;
	}

	entry = ldap_first_entry(conn->handle, result);
	if (!entry) {
		ldap_get_option(conn->handle, LDAP_OPT_RESULT_CODE, &ldap_errno);
		REDEBUG("Failed retrieving entry: %s", ldap_err2string(ldap_errno));

		goto finish;
	}

	/*
	 *	Check for access.
	 */
	if (inst->userobj_access_attr) {
		rcode = rlm_ldap_check_access(inst, request, conn, entry);
		if (rcode != RLM_MODULE_OK) {
			goto finish;
		}
	}

	/*
	 *	Check if we need to cache group memberships
	 */
	if (inst->cacheable_group_dn || inst->cacheable_group_name) {
		if (inst->userobj_membership_attr) {
			rcode = rlm_ldap_cacheable_userobj(inst, request, &conn, entry, inst->userobj_membership_attr);
			if (rcode != RLM_MODULE_OK) {
				goto finish;
			}
		}

		rcode = rlm_ldap_cacheable_groupobj(inst, request, &conn);
		if (rcode != RLM_MODULE_OK) {
			goto finish;
		}
	}

#ifdef WITH_EDIR
	/*
	 *	We already have a Cleartext-Password.  Skip edir.
	 */
	if (fr_pair_find_by_num(request->control, 0, PW_CLEARTEXT_PASSWORD, TAG_ANY)) {
		goto skip_edir;
	}

	/*
	 *      Retrieve Universal Password if we use eDirectory
	 */
	if (inst->edir) {
		int res = 0;
		char password[256];
		size_t pass_size = sizeof(password);

		/*
		 *	Retrive universal password
		 */
		res = nmasldap_get_password(conn->handle, dn, password, &pass_size);
		if (res != 0) {
			REDEBUG("Failed to retrieve eDirectory password: (%i) %s", res, edir_errstr(res));
			rcode = RLM_MODULE_FAIL;

			goto finish;
		}

		/*
		 *	Add Cleartext-Password attribute to the request
		 */
		vp = radius_pair_create(request, &request->control, PW_CLEARTEXT_PASSWORD, 0);
		fr_pair_value_strcpy(vp, password);
		vp->vp_length = pass_size;

		if (RDEBUG_ENABLED3) {
			RDEBUG3("Added eDirectory password.  control:%s += '%s'", vp->da->name, vp->vp_strvalue);
		} else {
			RDEBUG2("Added eDirectory password");
		}

		if (inst->edir_autz) {
			RDEBUG2("Binding as user for eDirectory authorization checks");
			/*
			 *	Bind as the user
			 */
			conn->rebound = true;
			status = rlm_ldap_bind(inst, request, &conn, dn, vp->vp_strvalue, NULL, true, NULL, NULL, NULL);
			switch (status) {
			case LDAP_PROC_SUCCESS:
				rcode = RLM_MODULE_OK;
				RDEBUG("Bind as user '%s' was successful", dn);
				break;

			case LDAP_PROC_NOT_PERMITTED:
				rcode = RLM_MODULE_USERLOCK;
				goto finish;

			case LDAP_PROC_REJECT:
				rcode = RLM_MODULE_REJECT;
				goto finish;

			case LDAP_PROC_BAD_DN:
				rcode = RLM_MODULE_INVALID;
				goto finish;

			case LDAP_PROC_NO_RESULT:
				rcode = RLM_MODULE_NOTFOUND;
				goto finish;

			default:
				rcode = RLM_MODULE_FAIL;
				goto finish;
			};
		}
	}

skip_edir:
#endif

	/*
	 *	Apply ONE user profile, or a default user profile.
	 */
	if (inst->default_profile) {
		char const *profile;
		char profile_buff[1024];

		if (tmpl_expand(&profile, profile_buff, sizeof(profile_buff),
				request, inst->default_profile, NULL, NULL) < 0) {
			REDEBUG("Failed creating default profile string");

			rcode = RLM_MODULE_INVALID;
			goto finish;
		}

		switch (rlm_ldap_map_profile(inst, request, &conn, profile, &expanded)) {
		case RLM_MODULE_INVALID:
			rcode = RLM_MODULE_INVALID;
			goto finish;

		case RLM_MODULE_FAIL:
			rcode = RLM_MODULE_FAIL;
			goto finish;

		case RLM_MODULE_UPDATED:
			rcode = RLM_MODULE_UPDATED;
			/* FALL-THROUGH */
		default:
			break;
		}
	}

	/*
	 *	Apply a SET of user profiles.
	 */
	if (inst->profile_attr) {
		values = ldap_get_values_len(conn->handle, entry, inst->profile_attr);
		if (values != NULL) {
			for (i = 0; values[i] != NULL; i++) {
				rlm_rcode_t ret;
				char *value;

				value = rlm_ldap_berval_to_string(request, values[i]);
				ret = rlm_ldap_map_profile(inst, request, &conn, value, &expanded);
				talloc_free(value);
				if (ret == RLM_MODULE_FAIL) {
					ldap_value_free_len(values);
					rcode = ret;
					goto finish;
				}

			}
			ldap_value_free_len(values);
		}
	}

	if (inst->user_map || inst->valuepair_attr) {
		RDEBUG("Processing user attributes");
		RINDENT();
		if (rlm_ldap_map_do(inst, request, conn->handle, &expanded, entry) > 0) rcode = RLM_MODULE_UPDATED;
		REXDENT();
		rlm_ldap_check_reply(inst, request, conn);
	}

finish:
	talloc_free(expanded.ctx);
	if (result) ldap_msgfree(result);
	mod_conn_release(inst, request, conn);

	return rcode;
}

/** Modify user's object in LDAP
 *
 * Process a modifcation map to update a user object in the LDAP directory.
 *
 * @param inst rlm_ldap instance.
 * @param request Current request.
 * @param section that holds the map to process.
 * @return one of the RLM_MODULE_* values.
 */
static rlm_rcode_t user_modify(rlm_ldap_t const *inst, REQUEST *request, ldap_acct_section_t *section)
{
	rlm_rcode_t	rcode = RLM_MODULE_OK;
	ldap_rcode_t	status;

	ldap_handle_t	*conn = NULL;

	LDAPMod		*mod_p[LDAP_MAX_ATTRMAP + 1], mod_s[LDAP_MAX_ATTRMAP];
	LDAPMod		**modify = mod_p;

	char		*passed[LDAP_MAX_ATTRMAP * 2];
	int		i, total = 0, last_pass = 0;

	char 		*expanded[LDAP_MAX_ATTRMAP];
	int		last_exp = 0;

	char const	*attr;
	char const	*value;

	char const	*dn;
	/*
	 *	Build our set of modifications using the update sections in
	 *	the config.
	 */
	CONF_ITEM  	*ci;
	CONF_PAIR	*cp;
	CONF_SECTION 	*cs;
	FR_TOKEN	op;
	char		path[FR_MAX_STRING_LEN];

	char		*p = path;

	rad_assert(section);

	/*
	 *	Locate the update section were going to be using
	 */
	if (section->reference[0] != '.') {
		*p++ = '.';
	}

	if (xlat_eval(p, (sizeof(path) - (p - path)) - 1, request, section->reference, NULL, NULL) < 0) {
		goto error;
	}

	ci = cf_reference_item(NULL, section->cs, path);
	if (!ci) {
		goto error;
	}

	if (!cf_item_is_section(ci)){
		REDEBUG("Reference must resolve to a section");

		goto error;
	}

	cs = cf_section_sub_find(cf_item_to_section(ci), "update");
	if (!cs) {
		REDEBUG("Section must contain 'update' subsection");

		goto error;
	}

	/*
	 *	Iterate over all the pairs, building our mods array
	 */
	for (ci = cf_item_find_next(cs, NULL); ci != NULL; ci = cf_item_find_next(cs, ci)) {
		bool do_xlat = false;

		if (total == LDAP_MAX_ATTRMAP) {
			REDEBUG("Modify map size exceeded");

			goto error;
		}

		if (!cf_item_is_pair(ci)) {
			REDEBUG("Entry is not in \"ldap-attribute = value\" format");

			goto error;
		}

		/*
		 *	Retrieve all the information we need about the pair
		 */
		cp = cf_item_to_pair(ci);
		value = cf_pair_value(cp);
		attr = cf_pair_attr(cp);
		op = cf_pair_operator(cp);

		if (!value || (*value == '\0')) {
			RDEBUG("Empty value string, skipping attribute \"%s\"", attr);

			continue;
		}

		switch (cf_pair_value_type(cp)) {
		case T_BARE_WORD:
		case T_SINGLE_QUOTED_STRING:
			break;

		case T_BACK_QUOTED_STRING:
		case T_DOUBLE_QUOTED_STRING:
			do_xlat = true;
			break;

		default:
			rad_assert(0);
			goto error;
		}

		if (op == T_OP_CMP_FALSE) {
			passed[last_pass] = NULL;
		} else if (do_xlat) {
			char *exp = NULL;

			if (xlat_aeval(request, &exp, request, value, NULL, NULL) <= 0) {
				RDEBUG("Skipping attribute \"%s\"", attr);

				talloc_free(exp);

				continue;
			}

			expanded[last_exp++] = exp;
			passed[last_pass] = exp;
		/*
		 *	Static strings
		 */
		} else {
			memcpy(&(passed[last_pass]), &value, sizeof(passed[last_pass]));
		}

		passed[last_pass + 1] = NULL;

		mod_s[total].mod_values = &(passed[last_pass]);

		last_pass += 2;

		switch (op) {
		/*
		 *  T_OP_EQ is *NOT* supported, it is impossible to
		 *  support because of the lack of transactions in LDAP
		 */
		case T_OP_ADD:
			mod_s[total].mod_op = LDAP_MOD_ADD;
			break;

		case T_OP_SET:
			mod_s[total].mod_op = LDAP_MOD_REPLACE;
			break;

		case T_OP_SUB:
		case T_OP_CMP_FALSE:
			mod_s[total].mod_op = LDAP_MOD_DELETE;
			break;

#ifdef LDAP_MOD_INCREMENT
		case T_OP_INCRM:
			mod_s[total].mod_op = LDAP_MOD_INCREMENT;
			break;
#endif
		default:
			REDEBUG("Operator '%s' is not supported for LDAP modify operations",
				fr_int2str(fr_tokens_table, op, "<INVALID>"));

			goto error;
		}

		/*
		 *	Now we know the value is ok, copy the pointers into
		 *	the ldapmod struct.
		 */
		memcpy(&(mod_s[total].mod_type), &attr, sizeof(mod_s[total].mod_type));

		mod_p[total] = &(mod_s[total]);
		total++;
	}

	if (total == 0) {
		rcode = RLM_MODULE_NOOP;
		goto release;
	}

	mod_p[total] = NULL;

	conn = mod_conn_get(inst, request);
	if (!conn) return RLM_MODULE_FAIL;


	dn = rlm_ldap_find_user(inst, request, &conn, NULL, false, NULL, &rcode);
	if (!dn || (rcode != RLM_MODULE_OK)) {
		goto error;
	}

	status = rlm_ldap_modify(inst, request, &conn, dn, modify, NULL, NULL);
	switch (status) {
	case LDAP_PROC_SUCCESS:
		break;

	case LDAP_PROC_REJECT:
	case LDAP_PROC_BAD_DN:
		rcode = RLM_MODULE_INVALID;
		break;

	default:
		rcode = RLM_MODULE_FAIL;
		break;
	};

release:
error:
	/*
	 *	Free up any buffers we allocated for xlat expansion
	 */
	for (i = 0; i < last_exp; i++) talloc_free(expanded[i]);

	mod_conn_release(inst, request, conn);

	return rcode;
}

static rlm_rcode_t mod_accounting(void *instance, UNUSED void *thread, REQUEST *request) CC_HINT(nonnull);
static rlm_rcode_t mod_accounting(void *instance, UNUSED void *thread, REQUEST *request)
{
	rlm_ldap_t const *inst = instance;

	if (inst->accounting) return user_modify(inst, request, inst->accounting);

	return RLM_MODULE_NOOP;
}

static rlm_rcode_t mod_post_auth(void *instance, UNUSED void *thread, REQUEST *request) CC_HINT(nonnull);
static rlm_rcode_t CC_HINT(nonnull) mod_post_auth(void *instance, UNUSED void *thread, REQUEST *request)
{
	rlm_ldap_t const *inst = instance;

	if (inst->postauth) {
		return user_modify(inst, request, inst->postauth);
	}

	return RLM_MODULE_NOOP;
}


/** Detach from the LDAP server and cleanup internal state.
 *
 */
static int mod_detach(void *instance)
{
	rlm_ldap_t *inst = instance;

#ifdef HAVE_LDAP_CREATE_SORT_CONTROL
	if (inst->userobj_sort_ctrl) ldap_control_free(inst->userobj_sort_ctrl);
#endif

	fr_connection_pool_free(inst->pool);
	talloc_free(inst->user_map);

	return 0;
}

/** Parse an accounting sub section.
 *
 * Allocate a new ldap_acct_section_t and write the config data into it.
 *
 * @param[in] inst rlm_ldap configuration.
 * @param[in] parent of the config section.
 * @param[out] config to write the sub section parameters to.
 * @param[in] comp The section name were parsing the config for.
 * @return
 *	- 0 on success.
 *	- < 0 on failure.
 */
static int parse_sub_section(rlm_ldap_t *inst, CONF_SECTION *parent, ldap_acct_section_t **config,
			     rlm_components_t comp)
{
	CONF_SECTION *cs;

	char const *name = section_type_value[comp].section;

	cs = cf_section_sub_find(parent, name);
	if (!cs) {
		DEBUG2("rlm_ldap (%s) - Couldn't find configuration for %s, will return NOOP for calls "
		       "from this section", inst->name, name);

		return 0;
	}

	*config = talloc_zero(inst, ldap_acct_section_t);
	if (cf_section_parse(cs, *config, acct_section_config) < 0) {
		ERROR("rlm_ldap (%s) - Failed parsing configuration for section %s", inst->name, name);

		return -1;
	}

	(*config)->cs = cs;

	return 0;
}

/** Bootstrap the module
 *
 * Define attributes.
 *
 * @param conf to parse.
 * @param instance configuration data.
 * @return
 *	- 0 on success.
 *	- < 0 on failure.
 */
static int mod_bootstrap(CONF_SECTION *conf, void *instance)
{
	rlm_ldap_t	*inst = instance;
	char		buffer[256];
	char const	*group_attribute;

	inst->name = cf_section_name2(conf);
	if (!inst->name) inst->name = cf_section_name1(conf);

	if (inst->group_attribute) {
		group_attribute = inst->group_attribute;
	} else if (cf_section_name2(conf)) {
		snprintf(buffer, sizeof(buffer), "%s-LDAP-Group", inst->name);
		group_attribute = buffer;
	} else {
		group_attribute = "LDAP-Group";
	}

	if (paircompare_register_byname(group_attribute, fr_dict_attr_by_num(NULL, 0, PW_USER_NAME),
					false, rlm_ldap_groupcmp, inst) < 0) {
		ERROR("Error registering group comparison: %s", fr_strerror());
		goto error;
	}

	inst->group_da = fr_dict_attr_by_name(NULL, group_attribute);

	/*
	 *	Setup the cache attribute
	 */
	if (inst->cache_attribute) {
		fr_dict_attr_flags_t flags;

		memset(&flags, 0, sizeof(flags));
		if (fr_dict_attr_add(NULL, fr_dict_root(fr_dict_internal), inst->cache_attribute, -1, PW_TYPE_STRING,
				     flags) < 0) {
			ERROR("Error creating cache attribute: %s", fr_strerror());
		error:
			return -1;

		}
		inst->cache_da = fr_dict_attr_by_name(NULL, inst->cache_attribute);
	} else {
		inst->cache_da = inst->group_da;	/* Default to the group_da */
	}

	xlat_register(inst, inst->name, ldap_xlat, rlm_ldap_escape_func, NULL, 0, XLAT_DEFAULT_BUF_LEN);
	xlat_register(inst, "ldap_escape", ldap_escape_xlat, NULL, NULL, 0, XLAT_DEFAULT_BUF_LEN);
	xlat_register(inst, "ldap_unescape", ldap_unescape_xlat, NULL, NULL, 0, XLAT_DEFAULT_BUF_LEN);
	map_proc_register(inst, inst->name, mod_map_proc, ldap_map_verify, 0);

	return 0;
}

/** Instantiate the module
 *
 * Creates a new instance of the module reading parameters from a configuration section.
 *
 * @param conf to parse.
 * @param instance configuration data.
 * @return
 *	- 0 on success.
 *	- < 0 on failure.
 */
static int mod_instantiate(CONF_SECTION *conf, void *instance)
{
	size_t		i;

	CONF_SECTION	*options, *update;
	rlm_ldap_t	*inst = instance;

	inst->cs = conf;

	options = cf_section_sub_find(conf, "options");
	if (!options || !cf_pair_find(options, "chase_referrals")) {
		inst->pool_inst.chase_referrals_unset = true;	 /* use OpenLDAP defaults */
	}

	/*
	 *	If the configuration parameters can't be parsed, then fail.
	 */
	if ((parse_sub_section(inst, conf, &inst->accounting, MOD_ACCOUNTING) < 0) ||
	    (parse_sub_section(inst, conf, &inst->postauth, MOD_POST_AUTH) < 0)) {
		cf_log_err_cs(conf, "Failed parsing configuration");

		goto error;
	}

	/*
	 *	Sanity checks for cacheable groups code.
	 */
	if (inst->cacheable_group_name && inst->groupobj_membership_filter) {
		if (!inst->groupobj_name_attr) {
			cf_log_err_cs(conf, "Configuration item 'group.name_attribute' must be set if cacheable "
				      "group names are enabled");

			goto error;
		}
	}

	/*
	 *	If we have a *pair* as opposed to a *section*
	 *	then the module is referencing another ldap module's
	 *	connection pool.
	 */
	if (!cf_pair_find(conf, "pool")) {
		if (!inst->pool_inst.server_str) {
			cf_log_err_cs(conf, "Configuration item 'server' must have a value");
			goto error;
		}
	}

#ifndef WITH_SASL
	if (inst->user_sasl.mech) {
		cf_log_err_cs(conf, "Configuration item 'user.sasl.mech' not supported.  "
			      "Linked libldap does not provide ldap_sasl_bind function");
		goto error;
	}

	if (inst->pool_inst.admin_sasl.mech) {
		cf_log_err_cs(conf, "Configuration item 'sasl.mech' not supported.  "
			      "Linked libldap does not provide ldap_sasl_interactive_bind function");
		goto error;
	}
#endif

#ifndef HAVE_LDAP_CREATE_SORT_CONTROL
	if (inst->userobj_sort_by) {
		cf_log_err_cs(conf, "Configuration item 'sort_by' not supported.  "
			      "Linked libldap does not provide ldap_create_sort_control function");
		goto error;
	}
#endif

#ifndef HAVE_LDAP_URL_PARSE
	if (inst->use_referral_credentials) {
		cf_log_err_cs(conf, "Configuration item 'use_referral_credentials' not supported.  "
			      "Linked libldap does not support URL parsing");
		goto error;
	}
#endif

	/*
	 *	Now iterate over all the 'server' config items
	 */
	for (i = 0; i < talloc_array_length(inst->pool_inst.server_str); i++) {
		char const *value = inst->pool_inst.server_str[i];
		size_t j;

		/*
		 *	Explicitly prevent multiple server definitions
		 *	being used in the same string.
		 */
		for (j = 0; j < talloc_array_length(value) - 1; j++) {
			switch (value[j]) {
			case ' ':
			case ',':
			case ';':
				cf_log_err_cs(conf, "Invalid character '%c' found in 'server' configuration item",
					      value[j]);
				goto error;

			default:
				continue;
			}
		}

#ifdef LDAP_CAN_PARSE_URLS
		/*
		 *	Split original server value out into URI, server and port
		 *	so whatever initialization function we use later will have
		 *	the server information in the format it needs.
		 */
		if (ldap_is_ldap_url(value)) {
			LDAPURLDesc	*ldap_url;
			bool		set_port_maybe = true;
			int		default_port = LDAP_PORT;
			char		*p;

			if (ldap_url_parse(value, &ldap_url)){
				cf_log_err_cs(conf, "Parsing LDAP URL \"%s\" failed", value);
			ldap_url_error:
				ldap_free_urldesc(ldap_url);
				return -1;
			}

			if (ldap_url->lud_dn && (ldap_url->lud_dn[0] != '\0')) {
				cf_log_err_cs(conf, "Base DN cannot be specified via server URL");
				goto ldap_url_error;
			}

			if (ldap_url->lud_attrs && ldap_url->lud_attrs[0]) {
				cf_log_err_cs(conf, "Attribute list cannot be specified via server URL");
				goto ldap_url_error;
			}

			/*
			 *	ldap_url_parse sets this to base by default.
			 */
			if (ldap_url->lud_scope != LDAP_SCOPE_BASE) {
				cf_log_err_cs(conf, "Scope cannot be specified via server URL");
				goto ldap_url_error;
			}
			ldap_url->lud_scope = -1;	/* Otherwise LDAP adds ?base */

			/*
			 *	The public ldap_url_parse function sets the default
			 *	port, so we have to discover whether a port was
			 *	included ourselves.
			 */
			if ((p = strchr(value, ']')) && (p[1] == ':')) {			/* IPv6 */
				set_port_maybe = false;
			} else if ((p = strchr(value, ':')) && (p = strchr(p + 1, ':'))) {	/* IPv4 */
				set_port_maybe = false;
			}

			/* We allow extensions */

#  ifdef HAVE_LDAP_INITIALIZE
			{
				char *url;

				/*
				 *	Figure out the default port from the URL
				 */
				if (ldap_url->lud_scheme) {
					if (strcmp(ldap_url->lud_scheme, "ldaps") == 0) {
						if (inst->pool_inst.start_tls == true) {
							cf_log_err_cs(conf, "ldaps:// scheme is not compatible "
								      "with 'start_tls'");
							goto ldap_url_error;
						}
						default_port = LDAPS_PORT;

					} else if (strcmp(ldap_url->lud_scheme, "ldapi") == 0) {
						set_port_maybe = false; /* Unix socket, no port */
					}
				}

				if (set_port_maybe) {
					/*
					 *	URL port overrides configured port.
					 */
					ldap_url->lud_port = inst->pool_inst.port;

					/*
					 *	If there's no URL port, then set it to the default
					 *	this is so debugging messages show explicitly
					 *	the port we're connecting to.
					 */
					if (!ldap_url->lud_port) ldap_url->lud_port = default_port;
				}

				url = ldap_url_desc2str(ldap_url);
				if (!url) {
					cf_log_err_cs(conf, "Failed recombining URL components");
					goto ldap_url_error;
				}
				inst->pool_inst.server = talloc_asprintf_append(inst->pool_inst.server, "%s ", url);
				free(url);
			}
#  else
			/*
			 *	No LDAP initialize function.  Can't specify a scheme.
			 */
			if (ldap_url->lud_scheme &&
			    ((strcmp(ldap_url->lud_scheme, "ldaps") == 0) ||
			    (strcmp(ldap_url->lud_scheme, "ldapi") == 0) ||
			    (strcmp(ldap_url->lud_scheme, "cldap") == 0))) {
				cf_log_err_cs(conf, "%s is not supported by linked libldap",
					      ldap_url->lud_scheme);
				return -1;
			}

			/*
			 *	URL port over-rides the configured
			 *	port.  But if there's no configured
			 *	port, we use the hard-coded default.
			 */
			if (set_port_maybe) {
				ldap_url->lud_port = inst->pool_inst.port;
				if (!ldap_url->lud_port) ldap_url->lud_port = default_port;
			}

			inst->pool_inst.server = talloc_asprintf_append(inst->pool_inst.server, "%s:%i ",
							      ldap_url->lud_host ? ldap_url->lud_host : "localhost",
							      ldap_url->lud_port);
#  endif
			/*
			 *	@todo We could set a few other top level
			 *	directives using the URL, like base_dn
			 *	and scope.
			 */
			ldap_free_urldesc(ldap_url);
		/*
		 *	We need to construct an LDAP URI
		 */
		} else
#endif	/* HAVE_LDAP_URL_PARSE && HAVE_LDAP_IS_LDAP_URL && LDAP_URL_DESC2STR */
		/*
		 *	If it's not an URL, or we don't have the functions necessary
		 *	to break apart the URL and recombine it, then just treat
		 *	server as a hostname.
		 */
		{
#ifdef HAVE_LDAP_INITIALIZE
			char	const *p;
			char	*q;
			int	port = 0;
			size_t	len;

			port = inst->pool_inst.port;

			/*
			 *	We don't support URLs if the library didn't provide
			 *	URL parsing functions.
			 */
			if (strchr(value, '/')) {
			bad_server_fmt:
#ifdef LDAP_CAN_PARSE_URLS
				cf_log_err_cs(conf, "Invalid 'server' entry, must be in format <server>[:<port>] or "
					      "an ldap URI (ldap|cldap|ldaps|ldapi)://<server>:<port>");
#else
				cf_log_err_cs(conf, "Invalid 'server' entry, must be in format <server>[:<port>]");
#endif
				return -1;
			}

			p = strrchr(value, ':');
			if (p) {
				port = (int)strtol((p + 1), &q, 10);
				if ((p == value) || ((p + 1) == q) || (*q != '\0')) goto bad_server_fmt;
				len = p - value;
			} else {
				len = strlen(value);
			}
			if (port == 0) port = LDAP_PORT;

			inst->pool_inst.server = talloc_asprintf_append(inst->pool_inst.server, "ldap://%.*s:%i ", (int) len, value, port);
#else
			/*
			 *	ldap_init takes port, which can be overridden by :port so
			 *	we don't need to do any parsing here.
			 */
			inst->pool_inst.server = talloc_asprintf_append(inst->pool_inst.server, "%s ", value);
#endif
		}
	}

	/*
	 *	inst->pool_inst.server be unset if connection pool sharing is used.
	 */
	if (inst->pool_inst.server) {
		inst->pool_inst.server[talloc_array_length(inst->pool_inst.server) - 2] = '\0';
		DEBUG4("rlm_ldap (%s) - LDAP server string: %s", inst->name, inst->pool_inst.server);
	}

#ifdef LDAP_OPT_X_TLS_NEVER
	/*
	 *	Workaround for servers which support LDAPS but not START TLS
	 */
	if (inst->pool_inst.port == LDAPS_PORT || inst->pool_inst.tls_mode) {
		inst->pool_inst.tls_mode = LDAP_OPT_X_TLS_HARD;
	} else {
		inst->pool_inst.tls_mode = 0;
	}
#endif

	/*
	 *	Convert dereference strings to enumerated constants
	 */
	if (inst->pool_inst.dereference_str) {
		inst->pool_inst.dereference = fr_str2int(ldap_dereference, inst->pool_inst.dereference_str, -1);
		if (inst->pool_inst.dereference < 0) {
			cf_log_err_cs(conf, "Invalid 'dereference' value \"%s\", expected 'never', 'searching', "
				      "'finding' or 'always'", inst->pool_inst.dereference_str);
			goto error;
		}
	}

#if LDAP_SET_REBIND_PROC_ARGS != 3
	/*
	 *	The 2-argument rebind doesn't take an instance variable.  Our rebind function needs the instance
	 *	variable for the username, password, etc.
	 */
	if (inst->rebind == true) {
		cf_log_err_cs(conf, "Cannot use 'rebind' configuration item as this version of libldap "
			      "does not support the API that we need");

		goto error;
	}
#endif

	/*
	 *	Convert scope strings to enumerated constants
	 */
	inst->userobj_scope = fr_str2int(ldap_scope, inst->userobj_scope_str, -1);
	if (inst->userobj_scope < 0) {
		cf_log_err_cs(conf, "Invalid 'user.scope' value \"%s\", expected 'sub', 'one'"
#ifdef LDAP_SCOPE_CHILDREN
			      ", 'base' or 'children'"
#else
			      " or 'base'"
#endif
			 , inst->userobj_scope_str);
		goto error;
	}

	inst->groupobj_scope = fr_str2int(ldap_scope, inst->groupobj_scope_str, -1);
	if (inst->groupobj_scope < 0) {
		cf_log_err_cs(conf, "Invalid 'group.scope' value \"%s\", expected 'sub', 'one'"
#ifdef LDAP_SCOPE_CHILDREN
			      ", 'base' or 'children'"
#else
			      " or 'base'"
#endif
			 , inst->groupobj_scope_str);
		goto error;
	}

	inst->clientobj_scope = fr_str2int(ldap_scope, inst->clientobj_scope_str, -1);
	if (inst->clientobj_scope < 0) {
		cf_log_err_cs(conf, "Invalid 'client.scope' value \"%s\", expected 'sub', 'one'"
#ifdef LDAP_SCOPE_CHILDREN
			      ", 'base' or 'children'"
#else
			      " or 'base'"
#endif
			 , inst->clientobj_scope_str);
		goto error;
	}

#ifdef HAVE_LDAP_CREATE_SORT_CONTROL
	/*
	 *	Build the server side sort control for user objects
	 */
	if (inst->userobj_sort_by) {
		LDAPSortKey	**keys;
		int		ret;
		char		*p;

		memcpy(&p, &inst->userobj_sort_by, sizeof(p));

		ret = ldap_create_sort_keylist(&keys, p);
		if (ret != LDAP_SUCCESS) {
			cf_log_err_cs(conf, "Invalid user.sort_by value \"%s\": %s",
				      inst->userobj_sort_by, ldap_err2string(ret));
			goto error;
		}

		/*
		 *	Always set the control as critical, if it's not needed
		 *	the user can comment it out...
		 */
		ret = ldap_create_sort_control(global_handle, keys, 1, &inst->userobj_sort_ctrl);
		ldap_free_sort_keylist(keys);
		if (ret != LDAP_SUCCESS) {
			ERROR("Failed creating server sort control: %s", ldap_err2string(ret));
			goto error;
		}
	}
#endif

	if (inst->pool_inst.tls_require_cert_str) {
#ifdef LDAP_OPT_X_TLS_NEVER
		/*
		 *	Convert cert strictness to enumerated constants
		 */
		inst->pool_inst.tls_require_cert = fr_str2int(ldap_tls_require_cert,
							      inst->pool_inst.tls_require_cert_str, -1);
		if (inst->pool_inst.tls_require_cert < 0) {
			cf_log_err_cs(conf, "Invalid 'tls.require_cert' value \"%s\", expected 'never', "
				      "'demand', 'allow', 'try' or 'hard'", inst->pool_inst.tls_require_cert_str);
			goto error;
		}
#else
		cf_log_err_cs(conf, "Modifying 'tls.require_cert' is not supported by current "
			      "version of libldap. Please upgrade or substitute current libldap and "
			      "rebuild this module");

		goto error;
#endif
	}

	/*
	 *	Build the attribute map
	 */
	update = cf_section_sub_find(inst->cs, "update");
	if (update && (map_afrom_cs(&inst->user_map, update,
				    PAIR_LIST_REPLY, PAIR_LIST_REQUEST, rlm_ldap_map_verify, inst,
				    LDAP_MAX_ATTRMAP) < 0)) {
		return -1;
	}

	/*
	 *	Set global options
	 */
	if (rlm_ldap_global_init(inst) < 0) goto error;

	/*
	 *	Initialize the socket pool.
	 */
	inst->pool = module_connection_pool_init(inst->cs, inst, mod_conn_create, NULL, NULL, NULL, NULL);
	if (!inst->pool) goto error;

	/*
	 *	Bulk load dynamic clients.
	 */
	if (inst->do_clients) {
		CONF_SECTION *cs, *map, *tmpl;

		cs = cf_section_sub_find(inst->cs, "client");
		if (!cs) {
			cf_log_err_cs(conf, "Told to load clients but no client section found");
			goto error;
		}

		map = cf_section_sub_find(cs, "attribute");
		if (!map) {
			cf_log_err_cs(cs, "Told to load clients but no attribute section found");
			goto error;
		}

		tmpl = cf_section_sub_find(cs, "template");

		if (rlm_ldap_client_load(inst, tmpl, map) < 0) {
			cf_log_err_cs(cs, "Error loading clients");

			return -1;
		}
	}

	return 0;

error:
	return -1;
}

static int mod_load(void)
{
	static LDAPAPIInfo info = { .ldapai_info_version = LDAP_API_INFO_VERSION };	/* static to quiet valgrind about this being uninitialised */
	int ldap_errno;

	/*
	 *	Only needs to be done once, prevents races in environment
	 *	initialisation within libldap.
	 *
	 *	See: https://github.com/arr2036/ldapperf/issues/2
	 */
#ifdef HAVE_LDAP_INITIALIZE
	ldap_initialize(&global_handle, "");
#else
	global_handle = ldap_init("", 0);
#endif

	ldap_errno = ldap_get_option(NULL, LDAP_OPT_API_INFO, &info);
	if (ldap_errno == LDAP_OPT_SUCCESS) {
		/*
		 *	Don't generate warnings if the compile type vendor name
		 *	is found within the link time vendor name.
		 *
		 *	This allows the server to be built against OpenLDAP but
		 *	run with Symas OpenLDAP.
		 */
		if (strcasestr(info.ldapai_vendor_name, LDAP_VENDOR_NAME) == NULL) {
			WARN("rlm_ldap - libldap vendor changed since the server was built");
			WARN("rlm_ldap - linked: %s, built: %s", info.ldapai_vendor_name, LDAP_VENDOR_NAME);
		}

		if (info.ldapai_vendor_version < LDAP_VENDOR_VERSION) {
			WARN("rlm_ldap - libldap older than the version the server was built against");
			WARN("rlm_ldap - linked: %i, built: %i",
			     info.ldapai_vendor_version, LDAP_VENDOR_VERSION);
		}

		INFO("rlm_ldap - libldap vendor: %s, version: %i", info.ldapai_vendor_name,
		     info.ldapai_vendor_version);

		ldap_memfree(info.ldapai_vendor_name);
		ldap_memfree(info.ldapai_extensions);
	} else {
		DEBUG("rlm_ldap - Falling back to build time libldap version info.  Query for LDAP_OPT_API_INFO "
		      "returned: %i", ldap_errno);
		INFO("rlm_ldap - libldap vendor: %s, version: %i.%i.%i", LDAP_VENDOR_NAME,
		     LDAP_VENDOR_VERSION_MAJOR, LDAP_VENDOR_VERSION_MINOR, LDAP_VENDOR_VERSION_PATCH);
	}

	return 0;
}

static void mod_unload(void)
{
	/*
	 *	Keeping the dummy ld around for the lifetime
	 *	of the module should always work,
	 *	irrespective of what changes happen in libldap.
	 */
#ifdef HAVE_LDAP_UNBIND_EXT_S
	ldap_unbind_ext_s(global_handle, NULL, NULL);
#else
	ldap_unbind_s(global_handle);
#endif
}

/* globally exported name */
extern rad_module_t rlm_ldap;
rad_module_t rlm_ldap = {
	.magic		= RLM_MODULE_INIT,
	.name		= "ldap",
	.type		= 0,
	.inst_size	= sizeof(rlm_ldap_t),
	.config		= module_config,
	.load		= mod_load,
	.unload		= mod_unload,
	.bootstrap	= mod_bootstrap,
	.instantiate	= mod_instantiate,
	.detach		= mod_detach,
	.methods = {
		[MOD_AUTHENTICATE]	= mod_authenticate,
		[MOD_AUTHORIZE]		= mod_authorize,
		[MOD_ACCOUNTING]	= mod_accounting,
		[MOD_POST_AUTH]		= mod_post_auth
	},
};
