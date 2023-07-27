#include <string.h>
#include "url.h"

/**
 * URI Schemes
 * http://en.wikipedia.org/wiki/URI_scheme
 */

static const char *URL_SCHEMES[] = {
  // official IANA registered schemes
  "aaa", "aaas", "about", "acap", "acct", "adiumxtra", "afp", "afs", "aim", "apt", "attachment", "aw",
  "beshare", "bitcoin", "bolo", "callto", "cap", "chrome", "crome-extension", "com-evenbrite-attendee",
  "cid", "coap", "coaps","content", "crid", "cvs", "data", "dav", "dict", "lna-playsingle", "dln-playcontainer",
  "dns", "dtn", "dvb", "ed2k", "facetime", "fax", "feed", "file", "finger", "fish","ftp", "geo", "gg","git",
  "gizmoproject", "go", "gopher", "gtalk", "h323", "hcp", "http", "https", "iax", "icap", "icon","im",
  "imap", "info", "ipn", "ipp", "irc", "irc6", "ircs", "iris", "iris.beep", "iris.xpc", "iris.xpcs","iris.lws",
  "itms", "jabber", "jar", "jms", "keyparc", "lastfm", "ldap", "ldaps", "magnet", "mailserver","mailto",
  "maps", "market", "message", "mid", "mms", "modem", "ms-help", "mssettings-power", "msnim", "msrp",
  "msrps", "mtqp", "mumble", "mupdate", "mvn", "news", "nfs", "ni", "nih", "nntp", "notes","oid",
  "paquelocktoken", "pack", "palm", "paparazzi", "pkcs11", "platform", "pop", "pres", "prospero", "proxy",
  "psyc","query", "reload", "res", "resource", "rmi", "rsync", "rtmp","rtsp",  "secondlife", "service","session",
  "sftp", "sgn", "shttp", "sieve", "sip", "sips", "skype", "smb", "sms", "snews", "snmp", "soap.beep","soap.beeps",
  "soldat", "spotify", "ssh", "steam", "svn", "tag", "teamspeak", "tel", "telnet", "tftp", "things","thismessage",
  "tn3270", "tip", "tv", "udp", "unreal", "urn", "ut2004", "vemmi","ventrilo", "videotex", "view-source", "wais","webcal",
  "ws", "wss", "wtai", "wyciwyg", "xcon", "xcon-userid", "xfire","xmlrpc.beep",  "xmlrpc.beeps", "xmpp", "xri","ymsgr",

  // unofficial schemes
  "javascript", "jdbc", "doi"
};

#ifndef HAVE_STRDUP
// non C99 standard functions
#if __POSIX_C_SOURCE__ < 200809L
char *
strdup (const char *str) {
  int n = strlen(str) + 1;
  char *dup = (char *) malloc(n);
  if (dup) strcpy(dup, str);
  return dup;
}
#endif
#endif


static char *
strff (const char* ptr, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    (void) *ptr++;
  }

  return strdup(ptr);
}


static char *
get_part (const char* url, const char *format, int l) {
  bool has = false;
  char *tmp = strdup(url);
  char *tmp_url = strdup(url);
  char *fmt_url = strdup(url);
  char *ret = NULL;

  if (!tmp || !tmp_url || !fmt_url)
    return NULL;

  strcpy(tmp, "");
  strcpy(fmt_url, "");

  // move pointer exactly the amount
  // of characters in the `prototcol` char
  // plus 3 characters that represent the `://`
  // part of the url
  char* fmt_url_new = strff(fmt_url, l);
  free(fmt_url);
  fmt_url = fmt_url_new;

  sscanf(fmt_url, format, tmp);

  if (0 != strcmp(tmp, tmp_url)) {
    has = true;
    ret = strdup(tmp);
  }

  free(tmp);
  free(tmp_url);
  free(fmt_url);

  return has? ret : NULL;
}

url_data_t*
url_parse (char* url) {
  url_data_t *data = (url_data_t *) calloc(1, sizeof(url_data_t));
  if (!data) return NULL;

  data->href = url;
  char *tmp_url = strdup(url);

  char *protocol = url_get_protocol(tmp_url);
  if (!protocol) {
    free(tmp_url);
    free(data);
    return NULL;
  }
  // length of protocol plus ://
  const size_t protocol_len = strlen(protocol) + 3;
  data->protocol = protocol;

  const bool is_ssh = url_is_ssh(protocol);

  size_t auth_len = 0;
  if (strstr(tmp_url, "@")) {
    data->auth = get_part(tmp_url, "%[^@]", protocol_len);
    auth_len = strlen(data->auth);
    if (data->auth) auth_len++;
  }

  char *hostname = (is_ssh)
    ? get_part(tmp_url, "%[^:]", protocol_len + auth_len)
    : get_part(tmp_url, "%[^/]", protocol_len + auth_len);

  if (!hostname) {
    free(tmp_url);
    url_free(data);
    return NULL;
  }
  const size_t hostname_len = strlen(hostname);
  char *tmp_hostname = strdup(hostname);
  data->hostname = hostname;

  char *host = (char *) malloc((strlen(tmp_hostname)+1));
  sscanf(tmp_hostname, "%[^:]", host);
  free(tmp_hostname);
  if (!host) {
    free(tmp_url);
    url_free(data);
    return NULL;
  }
  data->host = host;

  const size_t host_len = strlen(host);
  if (hostname_len > host_len) {
    data->port = strff(hostname, host_len + 1); // +1 for ':' char;
  } else {
    data->port = NULL;
  }

  char *tmp_path = (is_ssh)
    ? get_part(tmp_url, ":%s", protocol_len + auth_len + hostname_len)
    : get_part(tmp_url, "/%s", protocol_len + auth_len + hostname_len);

  char *path = (char *) malloc((strlen(tmp_path) + 2));
  if (!path) {
    free(tmp_url);
    url_free(data);
    return NULL;
  }
  const char *fmt = (is_ssh)? "%s" : "/%s";
  sprintf(path, fmt, tmp_path);
  data->path = path;

  char *pathname = (char *) malloc((strlen(tmp_path) + 2));
  free(tmp_path);
  if (!pathname) {
    free(tmp_url);
    url_free(data);
    return NULL;
  }
  strcat(pathname, "");
  tmp_path = strdup(path);
  sscanf(tmp_path, "%[^? | ^#]", pathname);
  const size_t pathname_len = strlen(pathname);
  data->pathname = pathname;

  char* tmp_path_new = strff(tmp_path, pathname_len);
  free(tmp_path);
  tmp_path = tmp_path_new;
  char* search = NULL;
  sscanf(tmp_path, "%m[^#]", &search);
  data->search = search;

  const size_t search_len = search ? strlen(search) : 0;
  free(tmp_path);

  if(search) {
    char* query = NULL;
    sscanf(search, "?%ms", &query);
    data->query = query;
  }

  char* hash = NULL;
  tmp_path = strff(path, pathname_len + search_len);
  sscanf(tmp_path, "%ms", &hash);
  data->hash = hash;
  free(tmp_path);
  free(tmp_url);

  return data;
}

bool
url_is_protocol (const char* str) {
  const unsigned count = sizeof(URL_SCHEMES) / sizeof(URL_SCHEMES[0]);

  for (unsigned i = 0; i < count; ++i) {
    if (0 == strcmp(URL_SCHEMES[i], str)) {
      return true;
    }
  }

  return false;
}

bool
url_is_ssh (const char* str) {
  if (0 == strcmp(str, "ssh") || 0 == strcmp(str, "git")) {
    return true;
  }
  return false;
}

char *
url_get_protocol (const char* url) {
  char *protocol = (char *) malloc(URL_PROTOCOL_MAX_LENGTH);
  if (!protocol) return NULL;

  sscanf(url, "%[^://]", protocol);
  if (url_is_protocol(protocol)) return protocol;

  free(protocol);
  return NULL;
}


char *
url_get_auth (const char* url) {
  char *protocol = url_get_protocol(url);
  if (!protocol) return NULL;
  const size_t l = strlen(protocol) + 3;
  free(protocol);
  return get_part(url, "%[^@]", l);
}

char *
url_get_hostname (const char* url) {
  size_t l = 3;
  char *protocol = url_get_protocol(url);
  char *tmp_protocol = strdup(protocol);
  char *auth = url_get_auth(url);

  if (!protocol) return NULL;
  if (auth) {
    l += strlen(auth) + 1; // add one @ symbol
    free(auth);
  }

  l += strlen(protocol);

  free(protocol);

  char * hostname = url_is_ssh(tmp_protocol)
    ? get_part(url, "%[^:]", l)
    : get_part(url, "%[^/]", l);
  free(tmp_protocol);
  return hostname;
}

char *
url_get_host (const char* url) {
  char *host = NULL;
  char *hostname = url_get_hostname(url);

  if (!hostname) return NULL;

  sscanf(hostname, "%m[^:]", &host);

  free(hostname);

  return host;
}

char *
url_get_pathname (const char* url) {
  char *path = url_get_path(url);
  char *pathname = NULL;

  if (!path) return NULL;

  sscanf(path, "%m[^?]", &pathname);

  free(path);

  return pathname;
}

char *
url_get_path (const char* url) {
  size_t l = 3;
  char *protocol = url_get_protocol(url);
  char *auth     = url_get_auth(url);
  char *hostname = url_get_hostname(url);


  if (!protocol || !hostname)
    return NULL;

  const bool is_ssh = url_is_ssh(protocol);

  l += strlen(protocol) + strlen(hostname);

  if (auth) l+= strlen(auth) +1; // @ symbol

  char* tmp_path = (is_ssh)
    ? get_part(url, ":%s", l)
    : get_part(url, "/%s", l);

  const char *fmt = (is_ssh)? "%s" : "/%s";
  char *path = (char *) malloc(strlen(tmp_path)+2);
  sprintf(path, fmt, tmp_path);

  free(auth);
  free(protocol);
  free(hostname);
  free(tmp_path);

  return path;
}

char *
url_get_search (const char* url) {
  char *path = url_get_path(url);
  char *pathname = url_get_pathname(url);

  if (!path) return NULL;

  char *search = NULL;
  sscanf(path + strlen(pathname), "%m[^#]", &search);

  free(path);
  free(pathname);

  return search;
}

char *
url_get_query (const char* url) {
  char *search = url_get_search(url);
  char *query = NULL;
  if (!search) return NULL;

  sscanf(search, "?%ms", &query);
  free(search);
  return query;
}

char *
url_get_hash (const char* url) {
  char *path = url_get_path(url);
  if (!path) return NULL;

  char *pathname = url_get_pathname(url);
  if (!pathname) {
    free(path);
    return NULL;
  }
  
  char *search = url_get_search(url);

  const size_t pathname_len = strlen(pathname);
  const size_t search_len   = strlen(search);
  char *tmp_path = strff(path, pathname_len + search_len);

  char* hash = NULL;
  sscanf(tmp_path, "%ms", &hash);
//  tmp_path = strrwd(tmp_path, pathname_len + search_len);
  free(tmp_path);
  free(pathname);
  free(path);
  free(search);

  return hash;
}

char *
url_get_port (const char* url) {
  char *port = NULL;
  char *hostname = url_get_hostname(url);
  char *host = url_get_host(url);
  if (!hostname) return NULL;

  sscanf(hostname + strlen(host) + 1, "%ms", &port);
  free(hostname);
  free(host);
  return port;
}

void
url_inspect (char* url) {
  url_data_inspect(url_parse(url));
}


#define PRINT_MEMBER(member)  do{ \
	if(data->member) \
		printf("    ." #member ": \"%s\"\n", data->member);  \
	else   \
		printf("    ." #member ": (NULL)\n");  \
	}while(0)

void
url_data_inspect (const url_data_t* data) {
  printf("#url =>\n");
  PRINT_MEMBER(href);
  PRINT_MEMBER(protocol);
  PRINT_MEMBER(host);
  PRINT_MEMBER(auth);
  PRINT_MEMBER(hostname);
  PRINT_MEMBER(pathname);
  PRINT_MEMBER(search);
  PRINT_MEMBER(path);
  PRINT_MEMBER(hash);
  PRINT_MEMBER(query);
  PRINT_MEMBER(port);
}

void
url_free (url_data_t *data) {
  if (!data) return;
  free(data->auth);
  free(data->protocol);
  free(data->hostname);
  free(data->host);
  free(data->pathname);
  free(data->path);
  free(data->hash);
  free(data->port);
  free(data->search);
  free(data->query);
  free(data);
}
