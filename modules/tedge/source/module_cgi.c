// CGI for the thin-edge.io Router App configuration page.
//
// A single binary is symlinked (by source/Makefile) as www/index.cgi,
// www/set.cgi, www/status.cgi and www/slog.cgi; main() dispatches on argv[0].
// The router's httpd serves /opt/tedge/www/ and enforces login via the
// www/.htpasswd -> /etc/htpasswd symlink; um_cgi_query_ok() adds the CSRF check.
//
//   index.cgi  - render the Cumulocity connection form
//   set.cgi    - validate + save settings, then run "etc/init restart"
//   cert.cgi   - render the "upload self-signed certificate to Cumulocity" form
//   certup.cgi - run "etc/init upload-cert" (tedge cert upload c8y) + show output
//   status.cgi - show daemon status, the active Cumulocity identity and the
//                recent mapper log
//   slog.cgi   - the standard system-log viewer, filtered to this module

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "um_cgi.h"
#include "um_html.h"
#include "um_process.h"

#include "module.h"
#include "module_cfg.h"

// **************************************************************************

// structure of main menu (NULL url = section heading)
static const um_menu_item_t MENU[] = {
  { "Status"        , NULL               },
  { "Overview"      , "status.cgi"       },
  { "System Log"    , "slog.cgi"         },
  { "Configuration" , NULL               },
  { "Cumulocity IoT", "index.cgi"        },
  { "Upload Certificate", "cert.cgi"     },
  { "Administration", NULL               },
  { "Return"        , "../../module.cgi" },
  { NULL            , NULL               }
};

// options for the registration / authentication mode (MOD_TEDGE_CA)
static const um_option_str_t CA_MODE[] = {
  { "Cumulocity CA - one-time password (recommended)", "c8y-ca"      },
  { "Basic auth - device user and password"          , "basic"       },
  { "Self-signed certificate (manual upload)"        , "self-signed" },
  { NULL                                             , NULL          }
};

// **************************************************************************

// client-side validation: a Cumulocity URL is required to enable the service
static const char *JAVASCRIPT_BEGIN =
  "function CheckForm() {\n"
  "  if (document.f.enabled.checked &&\n"
  "      document.f.c8y_url.value.replace(/^\\s+|\\s+$/g, '') == '') {\n"
  "    return Error(\"A Cumulocity URL is required to enable thin-edge.io.\",\n"
  "                 document.f.c8y_url);\n"
  "  }\n"
  "  return true;\n"
  "}\n";

static const char *JAVASCRIPT_END =
  "document.f.onsubmit = CheckForm;\n"
  "document.f.enabled.focus();\n";

// **************************************************************************
// main function of CGI script "index.cgi"
static void main_index(void)
{
  module_cfg_t          cfg;

  module_cfg_load(&cfg);

  um_html_page_begin(MODULE_TITLE);

  um_html_form_begin(MODULE_TITLE, "Cumulocity IoT Connection", "set.cgi", 0, JAVASCRIPT_BEGIN, MENU);

  um_html_integrity(MODULE_SETTINGS);

  um_html_table(2, 0);

  um_html_check_box("enabled", cfg.enabled);
  um_html_text("Start thin-edge.io automatically on boot");

  um_html_table(2, 100);

  um_html_text("Cumulocity URL");
  um_html_input_str("c8y_url", cfg.c8y_url);

  um_html_text("Registration mode");
  um_html_select_str("ca", cfg.ca, CA_MODE);

  um_html_text("Device ID");
  um_html_input_str("device_id", cfg.device_id);

  um_html_text("One-time password");
  // Masked like the device/c8y passwords -- the OTP is a registration secret.
  // Empty pair-field: unlike a real password it must NOT be offered for
  // browser autocomplete/save (it is single-use).
  um_html_input_pwd("otp", cfg.otp, false, false, "");

  um_html_text("Device user");
  um_html_input_str("device_user", cfg.device_user);

  um_html_text("Device password");
  um_html_input_pwd("device_password", cfg.device_password, false, false, "device_user");

  um_html_form_break();

  um_html_table(1, 0);

  um_html_text("URL without the https:// prefix, e.g. mytenant.cumulocity.com. "
               "Leave the Device ID empty to keep the identity the router already has. "
               "The device user and password are used only in Basic auth mode. "
               "In Self-signed mode, the service creates a device certificate on the "
               "router; use the Upload Certificate page to upload it to Cumulocity.");

  um_html_form_break();

  um_html_table(1, 0);

  um_html_text("Changing the URL moves the router to another tenant, and changing the "
               "Device ID makes it a different device there. In Cumulocity CA mode both "
               "need a new registration: register the Device ID in the tenant (with a "
               "one-time password entered above) and the router connects as soon as that "
               "registration exists - it keeps retrying in the background, so the order "
               "does not matter. The previous certificate is archived, so switching back "
               "reconnects the router as the device it was before. The device left behind "
               "in the old tenant is not deleted. The Status page shows which tenant and "
               "device ID are currently in effect.");

  um_html_form_break();

  um_html_table(1, 0);

  um_html_submit("button", "Apply");

  um_html_form_end(JAVASCRIPT_END);

  um_html_page_end();
}

// **************************************************************************
// main function of CGI script "set.cgi"
static void main_set(void)
{
  module_cfg_t          cfg;
  int                   ok, input_ok;

  um_cgi_begin();

  um_html_page_begin(MODULE_TITLE);

  ok       = 0;
  input_ok = um_cgi_query_ok()                                       &&
             um_cgi_get_bool("enabled"        , &cfg.enabled       ) &&
             um_cgi_get_str ("c8y_url"        , &cfg.c8y_url        , 1) &&
             um_cgi_get_str ("ca"             , &cfg.ca             , 1) &&
             um_cgi_get_str ("device_id"      , &cfg.device_id      , 1) &&
             um_cgi_get_str ("otp"            , &cfg.otp            , 1) &&
             um_cgi_get_str ("device_user"    , &cfg.device_user    , 1) &&
             um_cgi_get_str ("device_password", &cfg.device_password, 0);

  // A URL is required to enable the service (mirrors the client-side check).
  if (input_ok && cfg.enabled && cfg.c8y_url[0] == '\0') {
    input_ok = 0;
  }

  if (input_ok) {
    if (module_cfg_save(&cfg)) {
      // Use "apply", not "restart": a full restart takes ~30s (tedge connect +
      // first-connect heal) and would block this CGI request, leaving the
      // browser on an empty/timed-out page. "apply" kicks the restart off in the
      // background and returns immediately.
      ok = !um_process_exec(MODULE_INIT, "apply");
    }
  }

  um_html_config_info_box(ok, input_ok, 0, "index.cgi");

  um_html_page_end();

  um_cgi_end();
}

// **************************************************************************

// client-side validation for the certificate-upload form: user and password
// are both required.
static const char *CERT_JAVASCRIPT_BEGIN =
  "function CheckForm() {\n"
  "  if (document.f.c8y_user.value.replace(/^\\s+|\\s+$/g, '') == '') {\n"
  "    return Error(\"A Cumulocity user is required.\", document.f.c8y_user);\n"
  "  }\n"
  "  if (document.f.c8y_password.value == '') {\n"
  "    return Error(\"A Cumulocity password is required.\", document.f.c8y_password);\n"
  "  }\n"
  "  return true;\n"
  "}\n";

static const char *CERT_JAVASCRIPT_END =
  "document.f.onsubmit = CheckForm;\n"
  "document.f.c8y_user.focus();\n";

// **************************************************************************
// main function of CGI script "cert.cgi"
static void main_cert(void)
{
  um_html_page_begin(MODULE_TITLE);

  um_html_form_begin(MODULE_TITLE, "Upload Certificate to Cumulocity", "certup.cgi", 0, CERT_JAVASCRIPT_BEGIN, MENU);

  um_html_table(1, 0);

  um_html_text("Upload the router's self-signed device certificate to Cumulocity's "
               "trusted certificates so the device can authenticate. This is the "
               "manual step for the \"self-signed\" registration mode; the certificate "
               "must already exist on the router (it is created automatically the first "
               "time the service starts in self-signed mode). Enter a Cumulocity user "
               "allowed to manage trusted certificates - the credentials are used only "
               "for this upload and are not stored.");

  um_html_form_break();

  um_html_table(2, 100);

  um_html_text("Cumulocity user");
  um_html_input_str("c8y_user", "");

  um_html_text("Cumulocity password");
  um_html_input_pwd("c8y_password", "", false, false, "c8y_user");

  um_html_form_break();

  um_html_table(1, 0);

  um_html_submit("button", "Upload to Cumulocity");

  um_html_form_end(CERT_JAVASCRIPT_END);

  um_html_page_end();
}

// **************************************************************************
// main function of CGI script "certup.cgi"
static void main_cert_upload(void)
{
  char                  *c8y_user, *c8y_password;
  int                   input_ok;

  um_cgi_begin();

  um_html_page_begin(MODULE_TITLE);

  um_html_form_begin(MODULE_TITLE, "Upload Certificate to Cumulocity", "cert.cgi", 0, NULL, MENU);

  input_ok = um_cgi_query_ok()                              &&
             um_cgi_get_str("c8y_user"    , &c8y_user    , 1) &&
             um_cgi_get_str("c8y_password", &c8y_password, 0);

  if (input_ok && c8y_user[0] != '\0' && c8y_password[0] != '\0') {
    // Hand the credentials to "tedge cert upload c8y" via the environment so
    // they never appear in the process list or logs. etc/init reads the user
    // from MOD_TEDGE_C8Y_USER; tedge reads the password from C8Y_PASSWORD.
    setenv("MOD_TEDGE_C8Y_USER", c8y_user, 1);
    setenv("C8Y_PASSWORD", c8y_password, 1);

    um_html_pre_head("Certificate upload");
    // stdin from /dev/null so a tedge password prompt fails fast (no hang).
    um_html_pre_proc(MODULE_INIT " upload-cert </dev/null 2>&1", true);
  } else {
    um_html_text("A Cumulocity user and password are required to upload the certificate.");
  }

  um_html_form_end(NULL);

  um_html_page_end();

  um_cgi_end();
}

// **************************************************************************
// main function of CGI script "status.cgi"
static void main_status(void)
{
  um_html_page_begin(MODULE_TITLE);

  um_html_form_begin(MODULE_TITLE, "thin-edge.io Status", "status.cgi", 0, NULL, MENU);

  um_html_pre_head("Daemon status");
  um_html_pre_proc(MODULE_INIT " status 2>&1", true);

  // What the settings ask for vs. what the device certificate actually says --
  // the first thing to look at when a device does not show up in the expected
  // tenant, or still carries its old ID (see etc/init, show_identity).
  um_html_pre_head("Cumulocity identity");
  um_html_pre_proc(MODULE_INIT " identity 2>&1", true);

  um_html_pre_head("Recent Cumulocity mapper log");
  um_html_pre_proc("tail -n 40 /var/log/tedge/tedge-mapper-c8y.log 2>/dev/null", true);

  um_html_form_end(NULL);

  um_html_page_end();
}

// **************************************************************************
// main function of CGI script "slog.cgi"
static void main_slog(void)
{
  um_html_system_log(MODULE_TITLE, MENU, MODULE_NAME);
}

// **************************************************************************
// main function
int main(int argc, char *argv[])
{
  const char            *name;

  if (argc > 0) {
    name = strrchr(argv[0], '/');
    name = name ? name + 1 : argv[0];
    if (!strcmp(name, "index.cgi")) {
      main_index();
    } else if (!strcmp(name, "set.cgi")) {
      main_set();
    } else if (!strcmp(name, "cert.cgi")) {
      main_cert();
    } else if (!strcmp(name, "certup.cgi")) {
      main_cert_upload();
    } else if (!strcmp(name, "status.cgi")) {
      main_status();
    } else if (!strcmp(name, "slog.cgi")) {
      main_slog();
    }
  }

  return 0;
}
