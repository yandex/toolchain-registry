#include "sshTelemetry.h"
void send_auth_telemetry(int status, const char *auth_type) { (void)status; (void)auth_type; }
void send_auth_method_telemetry(const char *auth_methods) { (void)auth_methods; }
void send_encryption_telemetry(const char *direction, const char *cipher,
    const char *kex, const char *mac, const char *comp, const char *host_key,
    const char **cproposal, const char **sproposal) {
    (void)direction; (void)cipher; (void)kex; (void)mac; (void)comp;
    (void)host_key; (void)cproposal; (void)sproposal;
}
void send_kex_exch_exit_code_telemetry(int exit_code) { (void)exit_code; }
void send_pubkey_telemetry(const char *status) { (void)status; }
void send_shell_telemetry(int pty, int shell_type) { (void)pty; (void)shell_type; }
void send_pubkey_sign_telemetry(const char *status) { (void)status; }
void send_ssh_connection_telemetry(const char *conn, const char *port) { (void)conn; (void)port; }
void send_sshd_connection_telemetry(const char *conn) { (void)conn; }
void send_ssh_version_telemetry(const char *version, const char *peer,
    const char *error) { (void)version; (void)peer; (void)error; }
