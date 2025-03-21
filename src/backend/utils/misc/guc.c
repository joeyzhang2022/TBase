/*--------------------------------------------------------------------
 * guc.c
 *
 * Support for grand unified configuration scheme, including SET
 * command, configuration file, and command line options.
 * See src/backend/utils/misc/README for more information.
 *
 *
 * Portions Copyright (c) 2012-2014, TransLattice, Inc.
 * Copyright (c) 2000-2017, PostgreSQL Global Development Group
 * Written by Peter Eisentraut <peter_e@gmx.net>.
 *
 * IDENTIFICATION
 *      src/backend/utils/misc/guc.c
 *
 *--------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <float.h>
#include <math.h>
#include <limits.h>
#include <unistd.h>
#include <sys/stat.h>
#ifdef HAVE_SYSLOG
#include <syslog.h>
#endif

#include "access/commit_ts.h"
#include "access/gin.h"
#ifdef PGXC
#include "access/gtm.h"
#include "pgxc/pgxc.h"
#endif
#include "access/rmgr.h"
#include "access/transam.h"
#include "access/twophase.h"
#include "access/xact.h"
#include "access/xlog_internal.h"
#include "access/heapam_xlog.h"
#include "access/lru.h"
#include "catalog/namespace.h"
#include "catalog/pg_authid.h"
#include "commands/async.h"
#include "commands/prepare.h"
#include "commands/user.h"
#include "commands/vacuum.h"
#include "commands/variable.h"
#include "commands/trigger.h"
#include "funcapi.h"
#include "libpq/auth.h"
#include "libpq/be-fsstubs.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "optimizer/cost.h"
#include "optimizer/geqo.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "parser/parse_expr.h"
#include "parser/parse_type.h"
#include "parser/parser.h"
#include "parser/scansup.h"
#include "pgstat.h"
#ifdef PGXC
#include "commands/tablecmds.h"
#include "commands/trigger.h"
#include "nodes/nodes.h"
#include "pgxc/execRemote.h"
#include "pgxc/locator.h"
#include "pgxc/planner.h"
#include "pgxc/poolmgr.h"
#include "pgxc/nodemgr.h"
#include "pgxc/xc_maintenance_mode.h"
#include "storage/procarray.h"
#endif
#ifdef XCP
#include "commands/sequence.h"
#include "parser/parse_utilcmd.h"
#include "pgxc/nodemgr.h"
#include "pgxc/squeue.h"
#include "utils/snapmgr.h"
#endif
#include "postmaster/autovacuum.h"
#include "postmaster/bgworker_internals.h"
#include "postmaster/bgwriter.h"
#include "postmaster/postmaster.h"
#include "postmaster/syslogger.h"
#include "postmaster/walwriter.h"
#include "replication/logicallauncher.h"
#include "replication/slot.h"
#include "replication/syncrep.h"
#include "replication/walreceiver.h"
#include "replication/walsender.h"
#include "storage/bufmgr.h"
#include "storage/dsm_impl.h"
#include "storage/standby.h"
#include "storage/fd.h"
#include "storage/pg_shmem.h"
#include "storage/proc.h"
#include "storage/predicate.h"
#include "tcop/tcopprot.h"
#include "tsearch/ts_cache.h"
#include "utils/builtins.h"
#include "utils/bytea.h"
#include "utils/guc_tables.h"
#include "utils/memutils.h"
#include "utils/pg_locale.h"
#include "utils/plancache.h"
#include "utils/portal.h"
#include "utils/ps_status.h"
#include "utils/rls.h"
#include "utils/snapmgr.h"
#include "utils/tzparser.h"
#include "utils/varlena.h"
#include "utils/xml.h"
#include "utils/syscache.h"
#ifdef __TBASE__
#include "optimizer/subselect.h"
#include "postmaster/pgarch.h"
#include "optimizer/planner.h"
#include "optimizer/pathnode.h"
#include "tcop/pquery.h"
#include "optimizer/plancat.h"
#include "parser/analyze.h"
#endif

#ifdef __AUDIT__
#include "audit/audit.h"
#include "postmaster/auditlogger.h"
#endif

#ifdef __AUDIT_FGA__
#include "audit/audit_fga.h"
#endif

#ifdef __STORAGE_SCALABLE__
#include "replication/logical_statistic.h"
#endif
#ifdef _MLS_
#include "utils/relcrypt.h"
#include "utils/datamask.h"
#endif
#ifdef __COLD_HOT__
#include "utils/ruleutils.h"
#include "executor/nodeAgg.h"
#include "catalog/pg_partition_interval.h"
#endif

#ifdef _PUB_SUB_RELIABLE_
#include "access/xlog.h"
#endif

#ifndef PG_KRB_SRVTAB
#define PG_KRB_SRVTAB ""
#endif

#define CONFIG_FILENAME "postgresql.conf"
#define HBA_FILENAME    "pg_hba.conf"
#define IDENT_FILENAME    "pg_ident.conf"

#ifdef EXEC_BACKEND
#define CONFIG_EXEC_PARAMS "global/config_exec_params"
#define CONFIG_EXEC_PARAMS_NEW "global/config_exec_params.new"
#endif

/*
 * Precision with which REAL type guc values are to be printed for GUC
 * serialization.
 */
#define REALTYPE_PRECISION 17

/* XXX these should appear in other modules' header files */
extern bool Log_disconnections;
extern int    CommitDelay;
extern int    CommitSiblings;
extern char *default_tablespace;
extern char *temp_tablespaces;
extern bool ignore_checksum_failure;
extern bool synchronize_seqscans;
extern bool enable_cold_hot_router_print;
#ifdef _PUB_SUB_RELIABLE_
static char * g_wal_stream_type_str;
#endif

#ifdef TRACE_SYNCSCAN
extern bool trace_syncscan;
#endif
#ifdef DEBUG_BOUNDED_SORT
extern bool optimize_bounded_sort;
#endif

#ifdef __TBASE__
extern bool    PoolConnectDebugPrint;
extern bool       GTMDebugPrint;
extern bool    g_GTM_skip_catalog;
extern bool       PoolerStuckExit;
extern BackendId CoordSessionBackendId;
extern bool    PlpgsqlDebugPrint;
/* used for get total size of session */
static int32 g_TotalMemorySize = 0;
extern bool    enable_parallel_ddl;
extern bool    enable_distinct_optimizer;
#endif
static int    GUC_check_errcode_value;

/* global variables for check hook support */
char       *GUC_check_errmsg_string;
char       *GUC_check_errdetail_string;
char       *GUC_check_errhint_string;

static void do_serialize(char **destptr, Size *maxbytes, const char *fmt,...) pg_attribute_printf(3, 4);

static void set_config_sourcefile(const char *name, char *sourcefile,
                      int sourceline);
static bool call_bool_check_hook(struct config_bool *conf, bool *newval,
                     void **extra, GucSource source, int elevel);
static bool call_int_check_hook(struct config_int *conf, int *newval,
                    void **extra, GucSource source, int elevel);
static bool call_uint_check_hook(struct config_uint *conf, uint *newval,
                    void **extra, GucSource source, int elevel);
static bool call_real_check_hook(struct config_real *conf, double *newval,
                     void **extra, GucSource source, int elevel);
static bool call_string_check_hook(struct config_string *conf, char **newval,
                       void **extra, GucSource source, int elevel);
static bool call_enum_check_hook(struct config_enum *conf, int *newval,
                     void **extra, GucSource source, int elevel);

static bool check_log_destination(char **newval, void **extra, GucSource source);
static void assign_log_destination(const char *newval, void *extra);

static bool check_wal_consistency_checking(char **newval, void **extra,
                               GucSource source);
static void assign_wal_consistency_checking(const char *newval, void *extra);

#ifdef HAVE_SYSLOG
static int    syslog_facility = LOG_LOCAL0;
#else
static int    syslog_facility = 0;
#endif

static void assign_syslog_facility(int newval, void *extra);
static void assign_syslog_ident(const char *newval, void *extra);
static void assign_session_replication_role(int newval, void *extra);
static bool check_temp_buffers(int *newval, void **extra, GucSource source);
static bool check_bonjour(bool *newval, void **extra, GucSource source);
static bool check_ssl(bool *newval, void **extra, GucSource source);
static bool check_stage_log_stats(bool *newval, void **extra, GucSource source);
static bool check_log_stats(bool *newval, void **extra, GucSource source);
#ifdef PGXC
static bool check_pgxc_maintenance_mode(bool *newval, void **extra, GucSource source);
#endif
static bool check_canonical_path(char **newval, void **extra, GucSource source);
static bool check_timezone_abbreviations(char **newval, void **extra, GucSource source);
static void assign_timezone_abbreviations(const char *newval, void *extra);
static void pg_timezone_abbrev_initialize(void);
static const char *show_archive_command(void);
static void assign_tcp_keepalives_idle(int newval, void *extra);
static void assign_tcp_keepalives_interval(int newval, void *extra);
static void assign_tcp_keepalives_count(int newval, void *extra);
static const char *show_tcp_keepalives_idle(void);
static const char *show_tcp_keepalives_interval(void);
static const char *show_tcp_keepalives_count(void);
static bool check_maxconnections(int *newval, void **extra, GucSource source);
static bool check_max_worker_processes(int *newval, void **extra, GucSource source);
static bool check_autovacuum_max_workers(int *newval, void **extra, GucSource source);
static bool check_autovacuum_work_mem(int *newval, void **extra, GucSource source);
static bool check_effective_io_concurrency(int *newval, void **extra, GucSource source);
static void assign_effective_io_concurrency(int newval, void *extra);
static void assign_pgstat_temp_directory(const char *newval, void *extra);
static bool check_application_name(char **newval, void **extra, GucSource source);
static void assign_application_name(const char *newval, void *extra);
static bool check_cluster_name(char **newval, void **extra, GucSource source);
static const char *show_unix_socket_permissions(void);
static const char *show_log_file_mode(void);

#ifdef __AUDIT__
static const char *show_alog_file_mode(void);
#endif


/* Private functions in guc-file.l that need to be called from guc.c */
static ConfigVariable *ProcessConfigFileInternal(GucContext context,
                          bool applySettings, int elevel);

#ifdef XCP
static void strreplace_all(char *str, char *needle, char *replacement);
#endif

#ifdef __TBASE__
static bool set_warm_shared_buffer(bool *newval, void **extra, GucSource source);
static const char *show_total_memorysize(void);
#endif
#ifdef __COLD_HOT__
static void assign_cold_hot_partition_type(const char *newval, void *extra);

static void assign_manual_hot_date(const char *newval, void *extra);

static void assign_template_cold_date(const char *newval, void *extra);

static void assign_template_hot_date(const char *newval, void *extra);

#endif
#ifdef _MLS_
static void assign_default_locator_type(const char *newval, void *extra);
#endif
#ifdef _PUB_SUB_RELIABLE_
static void assign_wal_stream_type(const char *newval, void *extra);
#endif

/*
 * Options for enum values defined in this module.
 *
 * NOTE! Option values may not contain double quotes!
 */

static const struct config_enum_entry bytea_output_options[] = {
    {"escape", BYTEA_OUTPUT_ESCAPE, false},
    {"hex", BYTEA_OUTPUT_HEX, false},
    {NULL, 0, false}
};

/*
 * We have different sets for client and server message level options because
 * they sort slightly different (see "log" level)
 */
static const struct config_enum_entry client_message_level_options[] = {
    {"debug", DEBUG2, true},
    {"debug5", DEBUG5, false},
    {"debug4", DEBUG4, false},
    {"debug3", DEBUG3, false},
    {"debug2", DEBUG2, false},
    {"debug1", DEBUG1, false},
    {"log", LOG, false},
    {"info", INFO, true},
    {"notice", NOTICE, false},
    {"warning", WARNING, false},
    {"error", ERROR, false},
    {"fatal", FATAL, true},
    {"panic", PANIC, true},
    {NULL, 0, false}
};

static const struct config_enum_entry server_message_level_options[] = {
    {"debug", DEBUG2, true},
    {"debug5", DEBUG5, false},
    {"debug4", DEBUG4, false},
    {"debug3", DEBUG3, false},
    {"debug2", DEBUG2, false},
    {"debug1", DEBUG1, false},
    {"info", INFO, false},
    {"notice", NOTICE, false},
    {"warning", WARNING, false},
    {"error", ERROR, false},
    {"log", LOG, false},
    {"fatal", FATAL, false},
    {"panic", PANIC, false},
    {NULL, 0, false}
};

static const struct config_enum_entry intervalstyle_options[] = {
    {"postgres", INTSTYLE_POSTGRES, false},
    {"postgres_verbose", INTSTYLE_POSTGRES_VERBOSE, false},
    {"sql_standard", INTSTYLE_SQL_STANDARD, false},
    {"iso_8601", INTSTYLE_ISO_8601, false},
    {NULL, 0, false}
};

static const struct config_enum_entry log_error_verbosity_options[] = {
    {"terse", PGERROR_TERSE, false},
    {"default", PGERROR_DEFAULT, false},
    {"verbose", PGERROR_VERBOSE, false},
    {NULL, 0, false}
};

static const struct config_enum_entry log_statement_options[] = {
    {"none", LOGSTMT_NONE, false},
    {"ddl", LOGSTMT_DDL, false},
    {"mod", LOGSTMT_MOD, false},
    {"all", LOGSTMT_ALL, false},
    {NULL, 0, false}
};

static const struct config_enum_entry isolation_level_options[] = {
    {"serializable", XACT_SERIALIZABLE, false},
    {"repeatable read", XACT_REPEATABLE_READ, false},
    {"read committed", XACT_READ_COMMITTED, false},
    {"read uncommitted", XACT_READ_UNCOMMITTED, false},
    {NULL, 0}
};

static const struct config_enum_entry session_replication_role_options[] = {
    {"origin", SESSION_REPLICATION_ROLE_ORIGIN, false},
    {"replica", SESSION_REPLICATION_ROLE_REPLICA, false},
    {"local", SESSION_REPLICATION_ROLE_LOCAL, false},
    {NULL, 0, false}
};

static const struct config_enum_entry syslog_facility_options[] = {
#ifdef HAVE_SYSLOG
    {"local0", LOG_LOCAL0, false},
    {"local1", LOG_LOCAL1, false},
    {"local2", LOG_LOCAL2, false},
    {"local3", LOG_LOCAL3, false},
    {"local4", LOG_LOCAL4, false},
    {"local5", LOG_LOCAL5, false},
    {"local6", LOG_LOCAL6, false},
    {"local7", LOG_LOCAL7, false},
#else
    {"none", 0, false},
#endif
    {NULL, 0}
};

static const struct config_enum_entry track_function_options[] = {
    {"none", TRACK_FUNC_OFF, false},
    {"pl", TRACK_FUNC_PL, false},
    {"all", TRACK_FUNC_ALL, false},
    {NULL, 0, false}
};

static const struct config_enum_entry xmlbinary_options[] = {
    {"base64", XMLBINARY_BASE64, false},
    {"hex", XMLBINARY_HEX, false},
    {NULL, 0, false}
};

static const struct config_enum_entry xmloption_options[] = {
    {"content", XMLOPTION_CONTENT, false},
    {"document", XMLOPTION_DOCUMENT, false},
    {NULL, 0, false}
};

/*
 * Although only "on", "off", and "safe_encoding" are documented, we
 * accept all the likely variants of "on" and "off".
 */
static const struct config_enum_entry backslash_quote_options[] = {
    {"safe_encoding", BACKSLASH_QUOTE_SAFE_ENCODING, false},
    {"on", BACKSLASH_QUOTE_ON, false},
    {"off", BACKSLASH_QUOTE_OFF, false},
    {"true", BACKSLASH_QUOTE_ON, true},
    {"false", BACKSLASH_QUOTE_OFF, true},
    {"yes", BACKSLASH_QUOTE_ON, true},
    {"no", BACKSLASH_QUOTE_OFF, true},
    {"1", BACKSLASH_QUOTE_ON, true},
    {"0", BACKSLASH_QUOTE_OFF, true},
    {NULL, 0, false}
};

/*
 * Although only "on", "off", and "partition" are documented, we
 * accept all the likely variants of "on" and "off".
 */
static const struct config_enum_entry constraint_exclusion_options[] = {
    {"partition", CONSTRAINT_EXCLUSION_PARTITION, false},
    {"on", CONSTRAINT_EXCLUSION_ON, false},
    {"off", CONSTRAINT_EXCLUSION_OFF, false},
    {"true", CONSTRAINT_EXCLUSION_ON, true},
    {"false", CONSTRAINT_EXCLUSION_OFF, true},
    {"yes", CONSTRAINT_EXCLUSION_ON, true},
    {"no", CONSTRAINT_EXCLUSION_OFF, true},
    {"1", CONSTRAINT_EXCLUSION_ON, true},
    {"0", CONSTRAINT_EXCLUSION_OFF, true},
    {NULL, 0, false}
};

#ifdef PGXC
/*
 * Define remote connection types for PGXC
 */
static const struct config_enum_entry pgxc_conn_types[] = {
    {"application", REMOTE_CONN_APP, false},
    {"coordinator", REMOTE_CONN_COORD, false},
    {"datanode", REMOTE_CONN_DATANODE, false},
    {"gtm", REMOTE_CONN_GTM, false},
    {"gtmproxy", REMOTE_CONN_GTM_PROXY, false},
    {NULL, 0, false}
};
#endif

/*
 * Although only "on", "off", "remote_apply", "remote_write", and "local" are
 * documented, we accept all the likely variants of "on" and "off".
 */
static const struct config_enum_entry synchronous_commit_options[] = {
    {"local", SYNCHRONOUS_COMMIT_LOCAL_FLUSH, false},
    {"remote_write", SYNCHRONOUS_COMMIT_REMOTE_WRITE, false},
    {"remote_apply", SYNCHRONOUS_COMMIT_REMOTE_APPLY, false},
    {"on", SYNCHRONOUS_COMMIT_ON, false},
    {"off", SYNCHRONOUS_COMMIT_OFF, false},
    {"true", SYNCHRONOUS_COMMIT_ON, true},
    {"false", SYNCHRONOUS_COMMIT_OFF, true},
    {"yes", SYNCHRONOUS_COMMIT_ON, true},
    {"no", SYNCHRONOUS_COMMIT_OFF, true},
    {"1", SYNCHRONOUS_COMMIT_ON, true},
    {"0", SYNCHRONOUS_COMMIT_OFF, true},
    {NULL, 0, false}
};

/*
 * Although only "on", "off", "try" are documented, we accept all the likely
 * variants of "on" and "off".
 */
static const struct config_enum_entry huge_pages_options[] = {
    {"off", HUGE_PAGES_OFF, false},
    {"on", HUGE_PAGES_ON, false},
    {"try", HUGE_PAGES_TRY, false},
    {"true", HUGE_PAGES_ON, true},
    {"false", HUGE_PAGES_OFF, true},
    {"yes", HUGE_PAGES_ON, true},
    {"no", HUGE_PAGES_OFF, true},
    {"1", HUGE_PAGES_ON, true},
    {"0", HUGE_PAGES_OFF, true},
    {NULL, 0, false}
};

#ifdef XCP
/*
 * Set global-snapshot source. 'gtm' is default, but user can choose
 * 'coordinator' for performance improvement at the cost of reduced consistency
 */
static const struct config_enum_entry global_snapshot_source_options[] = {
    {"gtm", GLOBAL_SNAPSHOT_SOURCE_GTM, true},
    {"coordinator", GLOBAL_SNAPSHOT_SOURCE_COORDINATOR, true},
    {NULL, 0, false}
};
#endif

static const struct config_enum_entry force_parallel_mode_options[] = {
    {"off", FORCE_PARALLEL_OFF, false},
    {"on", FORCE_PARALLEL_ON, false},
    {"regress", FORCE_PARALLEL_REGRESS, false},
    {"true", FORCE_PARALLEL_ON, true},
    {"false", FORCE_PARALLEL_OFF, true},
    {"yes", FORCE_PARALLEL_ON, true},
    {"no", FORCE_PARALLEL_OFF, true},
    {"1", FORCE_PARALLEL_ON, true},
    {"0", FORCE_PARALLEL_OFF, true},
    {NULL, 0, false}
};

/*
 * password_encryption used to be a boolean, so accept all the likely
 * variants of "on", too. "off" used to store passwords in plaintext,
 * but we don't support that anymore.
 */
static const struct config_enum_entry password_encryption_options[] = {
    {"md5", PASSWORD_TYPE_MD5, false},
    {"scram-sha-256", PASSWORD_TYPE_SCRAM_SHA_256, false},
    {"on", PASSWORD_TYPE_MD5, true},
    {"true", PASSWORD_TYPE_MD5, true},
    {"yes", PASSWORD_TYPE_MD5, true},
    {"1", PASSWORD_TYPE_MD5, true},
    {NULL, 0, false}
};

#ifdef _SHARDING_
static const struct config_enum_entry shard_visible_modes[] = {
    {"visible", SHARD_VISIBLE_MODE_VISIBLE,false},
    {"hidden", SHARD_VISIBLE_MODE_HIDDEN, false},
    {"allin", SHARD_VISIBLE_MODE_ALL, false},
     {NULL, 0, false}
 };
#endif

#ifdef __TBASE__
/*
 * Although only "break", "continue" are documented, we
 * accept all the likely variants of "on" and "off".
 */
static const struct config_enum_entry archive_status_control_options[] = {
    {"continue", ARCHSTATUS_CONTINUE, false},
    {"break", ARCHSTATUS_BREAK, false},
    {"on", ARCHSTATUS_CONTINUE, true},
    {"off", ARCHSTATUS_BREAK, true},
    {NULL, ARCHSTATUS_CONTINUE, true}
};
#endif

/*
 * Options for enum values stored in other modules
 */
extern const struct config_enum_entry wal_level_options[];
extern const struct config_enum_entry archive_mode_options[];
extern const struct config_enum_entry sync_method_options[];
extern const struct config_enum_entry dynamic_shared_memory_options[];

/*
 * GUC option variables that are exported from this module
 */
bool        log_duration = false;
bool        Debug_print_plan = false;
bool        Debug_print_parse = false;
bool        Debug_print_rewritten = false;
bool        Debug_pretty_print = true;

bool        log_parser_stats = false;
bool        log_planner_stats = false;
bool        log_executor_stats = false;
bool        log_statement_stats = false;    /* this is sort of all three above
                                             * together */
#ifdef XCP
bool        log_gtm_stats = false;
bool        log_remotesubplan_stats = false;
#endif

bool        log_btree_build_stats = false;
char       *event_source;

bool        row_security;
bool        check_function_bodies = true;
bool        default_with_oids = false;
#ifdef _SHARDING_
bool        default_has_extent = false;
#endif
#ifdef _PG_ORCL_
bool        enable_oracle_compatible = false;
#endif
#ifdef __SUPPORT_DISTRIBUTED_TRANSACTION__
bool        support_oracle_compatible    = true;

#endif
#ifdef _MLS_
bool        g_CheckPassword = true;
bool        g_enable_crypt_parellel_debug = false;
bool        g_enable_crypt_check = false;
char *      g_default_locator_type = NULL;
#endif
#ifdef XCP
bool        random_collect_stats = true;
#endif

#ifdef _PUB_SUB_RELIABLE_
bool        g_replication_slot_debug = false;
#endif

#ifdef __TBASE__
int         query_delay = 0;
#endif

int            log_min_error_statement = ERROR;
int            log_min_messages = WARNING;
int            client_min_messages = NOTICE;
int            log_min_duration_statement = -1;
int            log_temp_files = -1;
int            trace_recovery_messages = LOG;

int            temp_file_limit = -1;

int            num_temp_buffers = 1024;

char       *cluster_name = "";
char       *ConfigFileName;
char       *HbaFileName;
char       *IdentFileName;
char       *external_pid_file;

char       *pgstat_temp_directory;

char       *application_name;

#ifdef _PG_ORCL_
char       *nls_date_format = "YYYY-MM-DD HH24:MI:SS";
char       *nls_timestamp_format = "YYYY-MM-DD HH24:MI:SS.US";
char       *nls_timestamp_tz_format = "YYYY-MM-DD HH24:MI:SS.US TZ";
char       *nls_sort_locale = NULL;
#endif

int            tcp_keepalives_idle;
int            tcp_keepalives_interval;
int            tcp_keepalives_count;

/*
 * SSL renegotiation was been removed in PostgreSQL 9.5, but we tolerate it
 * being set to zero (meaning never renegotiate) for backward compatibility.
 * This avoids breaking compatibility with clients that have never supported
 * renegotiation and therefore always try to zero it.
 */
int            ssl_renegotiation_limit;

/*
 * This really belongs in pg_shmem.c, but is defined here so that it doesn't
 * need to be duplicated in all the different implementations of pg_shmem.c.
 */
int            huge_pages;

/*
 * These variables are all dummies that don't do anything, except in some
 * cases provide the value for SHOW to display.  The real state is elsewhere
 * and is kept in sync by assign_hooks.
 */
static char *syslog_ident_str;
static bool session_auth_is_superuser;
#ifdef _MLS_
static bool g_is_mls_or_audit_user;
static bool g_is_inner_conn;
#endif
static double phony_random_seed;
static char *client_encoding_string;
static char *datestyle_string;
static char *locale_collate;
static char *locale_ctype;
static char *server_encoding_string;
static char *server_version_string;
static int    server_version_num;
static char *timezone_string;
static char *log_timezone_string;
static char *timezone_abbreviations_string;
static char *XactIsoLevel_string;
static char *data_directory;
static char *session_authorization_string;
#ifdef XCP
char *global_session_string;
#endif
static int    max_function_args;
static int    max_index_keys;
static int    max_identifier_length;
static int    block_size;
static int    segment_size;
static int    wal_block_size;
static bool data_checksums;
static int    wal_segment_size;
static bool integer_datetimes;
static bool assert_enabled;

#ifdef __TBASE__
bool g_enable_copy_silence = false;
bool g_enable_user_authority_force_check = false;
#endif

/* should be static, but commands/variable.c needs to get at this */
char       *role_string;

/*
 * Displayable names for context types (enum GucContext)
 *
 * Note: these strings are deliberately not localized.
 */
const char *const GucContext_Names[] =
{
     /* PGC_INTERNAL */ "internal",
     /* PGC_POSTMASTER */ "postmaster",
     /* PGC_SIGHUP */ "sighup",
     /* PGC_SU_BACKEND */ "superuser-backend",
     /* PGC_BACKEND */ "backend",
     /* PGC_SUSET */ "superuser",
     /* PGC_USERSET */ "user"
};

/*
 * Displayable names for source types (enum GucSource)
 *
 * Note: these strings are deliberately not localized.
 */
const char *const GucSource_Names[] =
{
     /* PGC_S_DEFAULT */ "default",
     /* PGC_S_DYNAMIC_DEFAULT */ "default",
     /* PGC_S_ENV_VAR */ "environment variable",
     /* PGC_S_FILE */ "configuration file",
     /* PGC_S_ARGV */ "command line",
     /* PGC_S_GLOBAL */ "global",
     /* PGC_S_DATABASE */ "database",
     /* PGC_S_USER */ "user",
     /* PGC_S_DATABASE_USER */ "database user",
     /* PGC_S_CLIENT */ "client",
     /* PGC_S_OVERRIDE */ "override",
     /* PGC_S_INTERACTIVE */ "interactive",
     /* PGC_S_TEST */ "test",
     /* PGC_S_SESSION */ "session"
};

/*
 * Displayable names for the groupings defined in enum config_group
 */
const char *const config_group_names[] =
{
    /* UNGROUPED */
    gettext_noop("Ungrouped"),
    /* FILE_LOCATIONS */
    gettext_noop("File Locations"),
    /* CONN_AUTH */
    gettext_noop("Connections and Authentication"),
    /* CONN_AUTH_SETTINGS */
    gettext_noop("Connections and Authentication / Connection Settings"),
    /* CONN_AUTH_SECURITY */
    gettext_noop("Connections and Authentication / Security and Authentication"),
    /* RESOURCES */
    gettext_noop("Resource Usage"),
    /* RESOURCES_MEM */
    gettext_noop("Resource Usage / Memory"),
    /* RESOURCES_DISK */
    gettext_noop("Resource Usage / Disk"),
    /* RESOURCES_KERNEL */
    gettext_noop("Resource Usage / Kernel Resources"),
    /* RESOURCES_VACUUM_DELAY */
    gettext_noop("Resource Usage / Cost-Based Vacuum Delay"),
    /* RESOURCES_BGWRITER */
    gettext_noop("Resource Usage / Background Writer"),
    /* RESOURCES_ASYNCHRONOUS */
    gettext_noop("Resource Usage / Asynchronous Behavior"),
    /* WAL */
    gettext_noop("Write-Ahead Log"),
    /* WAL_SETTINGS */
    gettext_noop("Write-Ahead Log / Settings"),
    /* WAL_CHECKPOINTS */
    gettext_noop("Write-Ahead Log / Checkpoints"),
    /* WAL_ARCHIVING */
    gettext_noop("Write-Ahead Log / Archiving"),
    /* REPLICATION */
    gettext_noop("Replication"),
    /* REPLICATION_SENDING */
    gettext_noop("Replication / Sending Servers"),
    /* REPLICATION_MASTER */
    gettext_noop("Replication / Master Server"),
    /* REPLICATION_STANDBY */
    gettext_noop("Replication / Standby Servers"),
    /* REPLICATION_SUBSCRIBERS */
    gettext_noop("Replication / Subscribers"),
    /* QUERY_TUNING */
    gettext_noop("Query Tuning"),
    /* QUERY_TUNING_METHOD */
    gettext_noop("Query Tuning / Planner Method Configuration"),
    /* QUERY_TUNING_COST */
    gettext_noop("Query Tuning / Planner Cost Constants"),
    /* QUERY_TUNING_GEQO */
    gettext_noop("Query Tuning / Genetic Query Optimizer"),
    /* QUERY_TUNING_OTHER */
    gettext_noop("Query Tuning / Other Planner Options"),
    /* LOGGING */
    gettext_noop("Reporting and Logging"),
    /* LOGGING_WHERE */
    gettext_noop("Reporting and Logging / Where to Log"),
    /* LOGGING_WHEN */
    gettext_noop("Reporting and Logging / When to Log"),
    /* LOGGING_WHAT */
    gettext_noop("Reporting and Logging / What to Log"),
    /* PROCESS_TITLE */
    gettext_noop("Process Title"),
    /* STATS */
    gettext_noop("Statistics"),
    /* STATS_MONITORING */
    gettext_noop("Statistics / Monitoring"),
    /* STATS_COLLECTOR */
    gettext_noop("Statistics / Query and Index Statistics Collector"),
    /* AUTOVACUUM */
    gettext_noop("Autovacuum"),
    /* CLIENT_CONN */
    gettext_noop("Client Connection Defaults"),
    /* CLIENT_CONN_STATEMENT */
    gettext_noop("Client Connection Defaults / Statement Behavior"),
    /* CLIENT_CONN_LOCALE */
    gettext_noop("Client Connection Defaults / Locale and Formatting"),
    /* CLIENT_CONN_PRELOAD */
    gettext_noop("Client Connection Defaults / Shared Library Preloading"),
    /* CLIENT_CONN_OTHER */
    gettext_noop("Client Connection Defaults / Other Defaults"),
    /* LOCK_MANAGEMENT */
    gettext_noop("Lock Management"),
    /* COMPAT_OPTIONS */
    gettext_noop("Version and Platform Compatibility"),
    /* COMPAT_OPTIONS_PREVIOUS */
    gettext_noop("Version and Platform Compatibility / Previous PostgreSQL Versions"),
    /* COMPAT_OPTIONS_CLIENT */
    gettext_noop("Version and Platform Compatibility / Other Platforms and Clients"),
    /* ERROR_HANDLING */
    gettext_noop("Error Handling"),
    /* PRESET_OPTIONS */
    gettext_noop("Preset Options"),
    /* CUSTOM_OPTIONS */
    gettext_noop("Customized Options"),
    /* DEVELOPER_OPTIONS */
    gettext_noop("Developer Options"),
#ifdef PGXC
    /* DATA_NODES */
    gettext_noop("Datanodes and Connection Pooling"),
    /* GTM */
    gettext_noop("GTM Connection"),
    /* COORDINATORS */
    gettext_noop("Coordinator Options"),
    /* XC_HOUSEKEEPING_OPTIONS */
    gettext_noop("XC Housekeeping Options"),
#endif
    /* help_config wants this array to be null-terminated */
    NULL
};

/*
 * Displayable names for GUC variable types (enum config_type)
 *
 * Note: these strings are deliberately not localized.
 */
const char *const config_type_names[] =
{
     /* PGC_BOOL */ "bool",
     /* PGC_INT */ "integer",
     /* PGC_UINT */ "unsigned integer",
     /* PGC_REAL */ "real",
     /* PGC_STRING */ "string",
     /* PGC_ENUM */ "enum"
};

/*
 * Unit conversion tables.
 *
 * There are two tables, one for memory units, and another for time units.
 * For each supported conversion from one unit to another, we have an entry
 * in the table.
 *
 * To keep things simple, and to avoid intermediate-value overflows,
 * conversions are never chained.  There needs to be a direct conversion
 * between all units (of the same type).
 *
 * The conversions from each base unit must be kept in order from greatest
 * to smallest unit; convert_from_base_unit() relies on that.  (The order of
 * the base units does not matter.)
 */
#define MAX_UNIT_LEN        3    /* length of longest recognized unit string */

typedef struct
{
    char        unit[MAX_UNIT_LEN + 1]; /* unit, as a string, like "kB" or
                                         * "min" */
    int            base_unit;        /* GUC_UNIT_XXX */
    int            multiplier;        /* If positive, multiply the value with this
                                 * for unit -> base_unit conversion.  If
                                 * negative, divide (with the absolute value) */
} unit_conversion;

/* Ensure that the constants in the tables don't overflow or underflow */
#if BLCKSZ < 1024 || BLCKSZ > (1024*1024)
#error BLCKSZ must be between 1KB and 1MB
#endif
#if XLOG_BLCKSZ < 1024 || XLOG_BLCKSZ > (1024*1024)
#error XLOG_BLCKSZ must be between 1KB and 1MB
#endif
#if XLOG_SEG_SIZE < (1024*1024) || XLOG_SEG_SIZE > (1024*1024*1024)
#error XLOG_SEG_SIZE must be between 1MB and 1GB
#endif

static const char *memory_units_hint = gettext_noop("Valid units for this parameter are \"kB\", \"MB\", \"GB\", and \"TB\".");

static const unit_conversion memory_unit_conversion_table[] =
{
    {"TB", GUC_UNIT_KB, 1024 * 1024 * 1024},
    {"GB", GUC_UNIT_KB, 1024 * 1024},
    {"MB", GUC_UNIT_KB, 1024},
    {"kB", GUC_UNIT_KB, 1},

    {"TB", GUC_UNIT_MB, 1024 * 1024},
    {"GB", GUC_UNIT_MB, 1024},
    {"MB", GUC_UNIT_MB, 1},
    {"kB", GUC_UNIT_MB, -1024},

    {"TB", GUC_UNIT_BLOCKS, (1024 * 1024 * 1024) / (BLCKSZ / 1024)},
    {"GB", GUC_UNIT_BLOCKS, (1024 * 1024) / (BLCKSZ / 1024)},
    {"MB", GUC_UNIT_BLOCKS, 1024 / (BLCKSZ / 1024)},
    {"kB", GUC_UNIT_BLOCKS, -(BLCKSZ / 1024)},

    {"TB", GUC_UNIT_XBLOCKS, (1024 * 1024 * 1024) / (XLOG_BLCKSZ / 1024)},
    {"GB", GUC_UNIT_XBLOCKS, (1024 * 1024) / (XLOG_BLCKSZ / 1024)},
    {"MB", GUC_UNIT_XBLOCKS, 1024 / (XLOG_BLCKSZ / 1024)},
    {"kB", GUC_UNIT_XBLOCKS, -(XLOG_BLCKSZ / 1024)},

    {""}                        /* end of table marker */
};

static const char *time_units_hint = gettext_noop("Valid units for this parameter are \"ms\", \"s\", \"min\", \"h\", and \"d\".");

static const unit_conversion time_unit_conversion_table[] =
{
    {"d", GUC_UNIT_MS, 1000 * 60 * 60 * 24},
    {"h", GUC_UNIT_MS, 1000 * 60 * 60},
    {"min", GUC_UNIT_MS, 1000 * 60},
    {"s", GUC_UNIT_MS, 1000},
    {"ms", GUC_UNIT_MS, 1},

    {"d", GUC_UNIT_S, 60 * 60 * 24},
    {"h", GUC_UNIT_S, 60 * 60},
    {"min", GUC_UNIT_S, 60},
    {"s", GUC_UNIT_S, 1},
    {"ms", GUC_UNIT_S, -1000},

    {"d", GUC_UNIT_MIN, 60 * 24},
    {"h", GUC_UNIT_MIN, 60},
    {"min", GUC_UNIT_MIN, 1},
    {"s", GUC_UNIT_MIN, -60},
    {"ms", GUC_UNIT_MIN, -1000 * 60},

    {""}                        /* end of table marker */
};

/*
 * Contents of GUC tables
 *
 * See src/backend/utils/misc/README for design notes.
 *
 * TO ADD AN OPTION:
 *
 * 1. Declare a global variable of type bool, int, double, or char*
 *      and make use of it.
 *
 * 2. Decide at what times it's safe to set the option. See guc.h for
 *      details.
 *
 * 3. Decide on a name, a default value, upper and lower bounds (if
 *      applicable), etc.
 *
 * 4. Add a record below.
 *
 * 5. Add it to src/backend/utils/misc/postgresql.conf.sample, if
 *      appropriate.
 *
 * 6. Don't forget to document the option (at least in config.sgml).
 *
 * 7. If it's a new GUC_LIST option you must edit pg_dumpall.c to ensure
 *      it is not single quoted at dump time.
 */


/******** option records follow ********/

static struct config_bool ConfigureNamesBool[] =
{
	{
		{"enable_seqscan", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enables the planner's use of sequential-scan plans."),
			NULL
		},
		&enable_seqscan,
		true,
		NULL, NULL, NULL
	},
	{
		{"enable_indexscan", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enables the planner's use of index-scan plans."),
			NULL
		},
		&enable_indexscan,
		true,
		NULL, NULL, NULL
	},
	{
		{"enable_indexonlyscan", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enables the planner's use of index-only-scan plans."),
			NULL
		},
		&enable_indexonlyscan,
		true,
		NULL, NULL, NULL
	},
	{
		{"enable_bitmapscan", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enables the planner's use of bitmap-scan plans."),
			NULL
		},
		&enable_bitmapscan,
		true,
		NULL, NULL, NULL
	},
	{
		{"enable_tidscan", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enables the planner's use of TID scan plans."),
			NULL
		},
		&enable_tidscan,
		true,
		NULL, NULL, NULL
	},
	{
		{"enable_sort", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enables the planner's use of explicit sort steps."),
			NULL
		},
		&enable_sort,
		true,
		NULL, NULL, NULL
	},
	{
		{"enable_hashagg", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enables the planner's use of hashed aggregation plans."),
			NULL
		},
		&enable_hashagg,
		true,
		NULL, NULL, NULL
	},
	{
		{"enable_material", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enables the planner's use of materialization."),
			NULL
		},
		&enable_material,
		true,
		NULL, NULL, NULL
	},
	{
		{"enable_nestloop", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enables the planner's use of nested-loop join plans."),
			NULL
		},
		&enable_nestloop,
		true,
		NULL, NULL, NULL
	},
#ifdef __TBASE__
	{
		{"enable_nestloop_suppression", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enables the selectivity hints when planning nested-loop joins."),
			NULL
		},
		&enable_nestloop_suppression,
		false,
		NULL, NULL, NULL
	},
#endif
	{
		{"enable_mergejoin", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enables the planner's use of merge join plans."),
			NULL
		},
		&enable_mergejoin,
		true,
		NULL, NULL, NULL
	},
	{
		{"enable_hashjoin", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enables the planner's use of hash join plans."),
			NULL
		},
		&enable_hashjoin,
		true,
		NULL, NULL, NULL
	},
#ifdef PGXC
    {
        {"enable_fast_query_shipping", PGC_USERSET, QUERY_TUNING_METHOD,
            gettext_noop("Enables the planner's use of fast query shipping to ship query directly to datanode."),
            NULL
        },
        &enable_fast_query_shipping,
        true,
        NULL, NULL, NULL
    },
    {
        {"loose_constraints", PGC_USERSET, COORDINATORS,
            gettext_noop("Relax enforcing of constraints"),
            gettext_noop("If enabled then constraints like foreign keys "
                         "are not enforced. It's the users responsibility "
                         "to maintain referential integrity at the application "
                         "level")
        },
        &loose_constraints,
        false,
        NULL, NULL, NULL
    },
#ifdef __COLD_HOT__
    {
        {"loose_unique_index", PGC_USERSET, COORDINATORS,
            gettext_noop("Relax enforcing of unique index"),
            NULL
        },
        &loose_unique_index,
        true,
        NULL, NULL, NULL
    },
#endif
    {
        {"gtm_backup_barrier", PGC_SUSET, QUERY_TUNING_METHOD,
            gettext_noop("Enables coordinator to report barrier id to GTM for backup."),
            NULL
        },
        &gtm_backup_barrier,
        false,
        NULL, NULL, NULL
    },
    {
        {"enable_datanode_row_triggers", PGC_POSTMASTER, DEVELOPER_OPTIONS,
            gettext_noop("Enables datanode-only ROW triggers"),
            NULL
        },
        &enable_datanode_row_triggers,
        false,
        NULL, NULL, NULL
    },
    
#endif
    {
        {"enable_gathermerge", PGC_USERSET, QUERY_TUNING_METHOD,
            gettext_noop("Enables the planner's use of gather merge plans."),
            NULL
        },
        &enable_gathermerge,
        true,
        NULL, NULL, NULL
    },
	{
		{"enable_partition_wise_join", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enables partition-wise join."),
			NULL
		},
		&enable_partition_wise_join,
		false,
		NULL, NULL, NULL
	},

    {
        {"geqo", PGC_USERSET, QUERY_TUNING_GEQO,
            gettext_noop("Enables genetic query optimization."),
            gettext_noop("This algorithm attempts to do planning without "
                         "exhaustive searching.")
        },
        &enable_geqo,
        true,
        NULL, NULL, NULL
    },
    {
        /* Not for general use --- used by SET SESSION AUTHORIZATION */
        {"is_superuser", PGC_INTERNAL, UNGROUPED,
            gettext_noop("Shows whether the current user is a superuser."),
            NULL,
            GUC_REPORT | GUC_NO_SHOW_ALL | GUC_NO_RESET_ALL | GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
        },
        &session_auth_is_superuser,
        false,
        NULL, NULL, NULL
    },
#ifdef _MLS_
    {
        /* Not for general use --- used by SET SESSION AUTHORIZATION */
        {"is_mls_or_audit_user", PGC_INTERNAL, UNGROUPED,
            gettext_noop("Shows whether the current user is a mls or audit user."),
            NULL,
            GUC_REPORT | GUC_NO_SHOW_ALL | GUC_NO_RESET_ALL | GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
        },
        &g_is_mls_or_audit_user,
        false,
        NULL, NULL, NULL
    },
    {
        /* Not for general use --- used by checking inner connection between pooler and nodes */
        {"inner_conn", PGC_USERSET, UNGROUPED,
            gettext_noop("inner connection between pooler and nodes."),
            NULL,
            GUC_REPORT | GUC_NO_SHOW_ALL | GUC_NO_RESET_ALL | GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
        },
        &g_is_inner_conn,
        false,
        NULL, NULL, NULL
    },
#endif
    {
        {"bonjour", PGC_POSTMASTER, CONN_AUTH_SETTINGS,
            gettext_noop("Enables advertising the server via Bonjour."),
            NULL
        },
        &enable_bonjour,
        false,
        check_bonjour, NULL, NULL
    },
    {
        {"track_commit_timestamp", PGC_POSTMASTER, REPLICATION,
            gettext_noop("Collects transaction commit time."),
            NULL
        },
        &track_commit_timestamp_guc,
        true,
        NULL, NULL, NULL
    },
    {
        {"ssl", PGC_SIGHUP, CONN_AUTH_SECURITY,
            gettext_noop("Enables SSL connections."),
            NULL
        },
        &EnableSSL,
        false,
        check_ssl, NULL, NULL
    },
    {
        {"ssl_prefer_server_ciphers", PGC_SIGHUP, CONN_AUTH_SECURITY,
            gettext_noop("Give priority to server ciphersuite order."),
            NULL
        },
        &SSLPreferServerCiphers,
        true,
        NULL, NULL, NULL
    },
    {
        {"fsync", PGC_SIGHUP, WAL_SETTINGS,
            gettext_noop("Forces synchronization of updates to disk."),
            gettext_noop("The server will use the fsync() system call in several places to make "
                         "sure that updates are physically written to disk. This insures "
                         "that a database cluster will recover to a consistent state after "
                         "an operating system or hardware crash.")
        },
        &enableFsync,
        true,
        NULL, NULL, NULL
    },
    {
        {"ignore_checksum_failure", PGC_SUSET, DEVELOPER_OPTIONS,
            gettext_noop("Continues processing after a checksum failure."),
            gettext_noop("Detection of a checksum failure normally causes PostgreSQL to "
                         "report an error, aborting the current transaction. Setting "
                         "ignore_checksum_failure to true causes the system to ignore the failure "
                         "(but still report a warning), and continue processing. This "
                         "behavior could cause crashes or other serious problems. Only "
                         "has an effect if checksums are enabled."),
            GUC_NOT_IN_SAMPLE
        },
        &ignore_checksum_failure,
        false,
        NULL, NULL, NULL
    },
    {
        {"zero_damaged_pages", PGC_SUSET, DEVELOPER_OPTIONS,
            gettext_noop("Continues processing past damaged page headers."),
            gettext_noop("Detection of a damaged page header normally causes PostgreSQL to "
                         "report an error, aborting the current transaction. Setting "
                         "zero_damaged_pages to true causes the system to instead report a "
                         "warning, zero out the damaged page, and continue processing. This "
                         "behavior will destroy data, namely all the rows on the damaged page."),
            GUC_NOT_IN_SAMPLE
        },
        &zero_damaged_pages,
        false,
        NULL, NULL, NULL
    },
    {
        {"full_page_writes", PGC_SIGHUP, WAL_SETTINGS,
            gettext_noop("Writes full pages to WAL when first modified after a checkpoint."),
            gettext_noop("A page write in process during an operating system crash might be "
                         "only partially written to disk.  During recovery, the row changes "
                         "stored in WAL are not enough to recover.  This option writes "
                         "pages when first modified after a checkpoint to WAL so full recovery "
                         "is possible.")
        },
        &fullPageWrites,
        true,
        NULL, NULL, NULL
    },

    {
        {"wal_log_hints", PGC_POSTMASTER, WAL_SETTINGS,
            gettext_noop("Writes full pages to WAL when first modified after a checkpoint, even for a non-critical modifications."),
            NULL
        },
        &wal_log_hints,
        false,
        NULL, NULL, NULL
    },

    {
        {"wal_compression", PGC_SUSET, WAL_SETTINGS,
            gettext_noop("Compresses full-page writes written in WAL file."),
            NULL
        },
        &wal_compression,
        false,
        NULL, NULL, NULL
    },

    {
        {"log_checkpoints", PGC_SIGHUP, LOGGING_WHAT,
            gettext_noop("Logs each checkpoint."),
            NULL
        },
        &log_checkpoints,
#ifdef _PG_REGRESS_
        true,
#else
        false,
#endif
        NULL, NULL, NULL
    },
    {
        {"log_connections", PGC_SU_BACKEND, LOGGING_WHAT,
            gettext_noop("Logs each successful connection."),
            NULL
        },
        &Log_connections,
        false,
        NULL, NULL, NULL
    },
    {
        {"log_disconnections", PGC_SU_BACKEND, LOGGING_WHAT,
            gettext_noop("Logs end of a session, including duration."),
            NULL
        },
        &Log_disconnections,
        false,
        NULL, NULL, NULL
    },
    {
        {"log_replication_commands", PGC_SUSET, LOGGING_WHAT,
            gettext_noop("Logs each replication command."),
            NULL
        },
        &log_replication_commands,
        false,
        NULL, NULL, NULL
    },
    {
        {"debug_assertions", PGC_INTERNAL, PRESET_OPTIONS,
            gettext_noop("Shows whether the running server has assertion checks enabled."),
            NULL,
            GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
        },
        &assert_enabled,
#ifdef USE_ASSERT_CHECKING
        true,
#else
        false,
#endif
        NULL, NULL, NULL
    },

    {
        {"exit_on_error", PGC_USERSET, ERROR_HANDLING_OPTIONS,
            gettext_noop("Terminate session on any error."),
            NULL
        },
        &ExitOnAnyError,
        false,
        NULL, NULL, NULL
    },
    {
        {"restart_after_crash", PGC_SIGHUP, ERROR_HANDLING_OPTIONS,
            gettext_noop("Reinitialize server after backend crash."),
            NULL
        },
        &restart_after_crash,
        true,
        NULL, NULL, NULL
    },

    {
        {"log_duration", PGC_SUSET, LOGGING_WHAT,
            gettext_noop("Logs the duration of each completed SQL statement."),
            NULL
        },
        &log_duration,
        false,
        NULL, NULL, NULL
    },
    {
        {"debug_print_parse", PGC_USERSET, LOGGING_WHAT,
            gettext_noop("Logs each query's parse tree."),
            NULL
        },
        &Debug_print_parse,
        false,
        NULL, NULL, NULL
    },
    {
        {"debug_print_rewritten", PGC_USERSET, LOGGING_WHAT,
            gettext_noop("Logs each query's rewritten parse tree."),
            NULL
        },
        &Debug_print_rewritten,
        false,
        NULL, NULL, NULL
    },
    {
        {"debug_print_plan", PGC_USERSET, LOGGING_WHAT,
            gettext_noop("Logs each query's execution plan."),
            NULL
        },
        &Debug_print_plan,
        false,
        NULL, NULL, NULL
    },
    {
        {"debug_pretty_print", PGC_USERSET, LOGGING_WHAT,
            gettext_noop("Indents parse and plan tree displays."),
            NULL
        },
        &Debug_pretty_print,
        true,
        NULL, NULL, NULL
    },
    {
        {"log_parser_stats", PGC_SUSET, STATS_MONITORING,
            gettext_noop("Writes parser performance statistics to the server log."),
            NULL
        },
        &log_parser_stats,
        false,
        check_stage_log_stats, NULL, NULL
    },
    {
        {"log_planner_stats", PGC_SUSET, STATS_MONITORING,
            gettext_noop("Writes planner performance statistics to the server log."),
            NULL
        },
        &log_planner_stats,
        false,
        check_stage_log_stats, NULL, NULL
    },
    {
        {"log_executor_stats", PGC_SUSET, STATS_MONITORING,
            gettext_noop("Writes executor performance statistics to the server log."),
            NULL
        },
        &log_executor_stats,
        false,
        check_stage_log_stats, NULL, NULL
    },
    {
        {"log_statement_stats", PGC_SUSET, STATS_MONITORING,
            gettext_noop("Writes cumulative performance statistics to the server log."),
            NULL
        },
        &log_statement_stats,
        false,
        check_log_stats, NULL, NULL
    },
#ifdef XCP
    {
        {"log_remotesubplan_stats", PGC_SUSET, STATS_MONITORING,
            gettext_noop("Writes remote subplan performance statistics to the server log."),
            NULL
        },
        &log_remotesubplan_stats,
        false,
        NULL, NULL, NULL
    },
    {
        {"log_gtm_stats", PGC_SUSET, STATS_MONITORING,
            gettext_noop("Writes GTM performance statistics to the server log."),
            NULL
        },
        &log_gtm_stats,
        false,
        NULL, NULL, NULL
    },
#endif
#ifdef BTREE_BUILD_STATS
    {
        {"log_btree_build_stats", PGC_SUSET, DEVELOPER_OPTIONS,
            gettext_noop("Logs system resource usage statistics (memory and CPU) on various B-tree operations."),
            NULL,
            GUC_NOT_IN_SAMPLE
        },
        &log_btree_build_stats,
        false,
        NULL, NULL, NULL
    },
#endif

    {
        {"track_activities", PGC_SUSET, STATS_COLLECTOR,
            gettext_noop("Collects information about executing commands."),
            gettext_noop("Enables the collection of information on the currently "
                         "executing command of each session, along with "
                         "the time at which that command began execution.")
        },
        &pgstat_track_activities,
        true,
        NULL, NULL, NULL
    },
    {
        {"track_counts", PGC_SUSET, STATS_COLLECTOR,
            gettext_noop("Collects statistics on database activity."),
            NULL
        },
        &pgstat_track_counts,
        true,
        NULL, NULL, NULL
    },
    {
        {"track_io_timing", PGC_SUSET, STATS_COLLECTOR,
            gettext_noop("Collects timing statistics for database I/O activity."),
            NULL
        },
        &track_io_timing,
        false,
        NULL, NULL, NULL
    },

    {
        {"update_process_title", PGC_SUSET, PROCESS_TITLE,
            gettext_noop("Updates the process title to show the active SQL command."),
            gettext_noop("Enables updating of the process title every time a new SQL command is received by the server.")
        },
        &update_process_title,
#ifdef WIN32
        false,
#else
        true,
#endif
        NULL, NULL, NULL
    },

    {
        {"autovacuum", PGC_SIGHUP, AUTOVACUUM,
            gettext_noop("Starts the autovacuum subprocess."),
            NULL
        },
        &autovacuum_start_daemon,
        true,
        NULL, NULL, NULL
    },

    {
        {"trace_notify", PGC_USERSET, DEVELOPER_OPTIONS,
            gettext_noop("Generates debugging output for LISTEN and NOTIFY."),
            NULL,
            GUC_NOT_IN_SAMPLE
        },
        &Trace_notify,
        false,
        NULL, NULL, NULL
    },

#ifdef LOCK_DEBUG
    {
        {"trace_locks", PGC_SUSET, DEVELOPER_OPTIONS,
            gettext_noop("Emits information about lock usage."),
            NULL,
            GUC_NOT_IN_SAMPLE
        },
        &Trace_locks,
        false,
        NULL, NULL, NULL
    },
    {
        {"trace_userlocks", PGC_SUSET, DEVELOPER_OPTIONS,
            gettext_noop("Emits information about user lock usage."),
            NULL,
            GUC_NOT_IN_SAMPLE
        },
        &Trace_userlocks,
        false,
        NULL, NULL, NULL
    },
    {
        {"trace_lwlocks", PGC_SUSET, DEVELOPER_OPTIONS,
            gettext_noop("Emits information about lightweight lock usage."),
            NULL,
            GUC_NOT_IN_SAMPLE
        },
        &Trace_lwlocks,
        false,
        NULL, NULL, NULL
    },
    {
        {"debug_deadlocks", PGC_SUSET, DEVELOPER_OPTIONS,
            gettext_noop("Dumps information about all current locks when a deadlock timeout occurs."),
            NULL,
            GUC_NOT_IN_SAMPLE
        },
        &Debug_deadlocks,
        false,
        NULL, NULL, NULL
    },
#endif

    {
        {"log_lock_waits", PGC_SUSET, LOGGING_WHAT,
            gettext_noop("Logs long lock waits."),
            NULL
        },
        &log_lock_waits,
        false,
        NULL, NULL, NULL
    },

    {
        {"log_hostname", PGC_SIGHUP, LOGGING_WHAT,
            gettext_noop("Logs the host name in the connection logs."),
            gettext_noop("By default, connection logs only show the IP address "
                         "of the connecting host. If you want them to show the host name you "
                         "can turn this on, but depending on your host name resolution "
                         "setup it might impose a non-negligible performance penalty.")
        },
        &log_hostname,
        false,
        NULL, NULL, NULL
    },
    {
        {"transform_null_equals", PGC_USERSET, COMPAT_OPTIONS_CLIENT,
            gettext_noop("Treats \"expr=NULL\" as \"expr IS NULL\"."),
            gettext_noop("When turned on, expressions of the form expr = NULL "
                         "(or NULL = expr) are treated as expr IS NULL, that is, they "
                         "return true if expr evaluates to the null value, and false "
                         "otherwise. The correct behavior of expr = NULL is to always "
                         "return null (unknown).")
        },
        &Transform_null_equals,
        false,
        NULL, NULL, NULL
    },
    {
        {"db_user_namespace", PGC_SIGHUP, CONN_AUTH_SECURITY,
            gettext_noop("Enables per-database user names."),
            NULL
        },
        &Db_user_namespace,
        false,
        NULL, NULL, NULL
    },
    {
        {"default_transaction_read_only", PGC_USERSET, CLIENT_CONN_STATEMENT,
            gettext_noop("Sets the default read-only status of new transactions."),
            NULL
        },
        &DefaultXactReadOnly,
        false,
        NULL, NULL, NULL
    },
    {
        {"transaction_read_only", PGC_USERSET, CLIENT_CONN_STATEMENT,
            gettext_noop("Sets the current transaction's read-only status."),
            NULL,
            GUC_NO_RESET_ALL | GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
        },
        &XactReadOnly,
        false,
        check_transaction_read_only, NULL, NULL
    },
    {
        {"default_transaction_deferrable", PGC_USERSET, CLIENT_CONN_STATEMENT,
            gettext_noop("Sets the default deferrable status of new transactions."),
            NULL
        },
        &DefaultXactDeferrable,
        false,
        NULL, NULL, NULL
    },
    {
        {"transaction_deferrable", PGC_USERSET, CLIENT_CONN_STATEMENT,
            gettext_noop("Whether to defer a read-only serializable transaction until it can be executed with no possible serialization failures."),
            NULL,
            GUC_NO_RESET_ALL | GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
        },
        &XactDeferrable,
        false,
        check_transaction_deferrable, NULL, NULL
    },
    {
        {"row_security", PGC_USERSET, CONN_AUTH_SECURITY,
            gettext_noop("Enable row security."),
            gettext_noop("When enabled, row security will be applied to all users.")
        },
        &row_security,
        true,
        NULL, NULL, NULL
    },
    {
        {"check_function_bodies", PGC_USERSET, CLIENT_CONN_STATEMENT,
            gettext_noop("Check function bodies during CREATE FUNCTION."),
            NULL
        },
        &check_function_bodies,
        true,
        NULL, NULL, NULL
    },
    {
        {"array_nulls", PGC_USERSET, COMPAT_OPTIONS_PREVIOUS,
            gettext_noop("Enable input of NULL elements in arrays."),
            gettext_noop("When turned on, unquoted NULL in an array input "
                         "value means a null value; "
                         "otherwise it is taken literally.")
        },
        &Array_nulls,
        true,
        NULL, NULL, NULL
    },
    {
        {"default_with_oids", PGC_USERSET, COMPAT_OPTIONS_PREVIOUS,
            gettext_noop("Create new tables with OIDs by default."),
            NULL
        },
        &default_with_oids,
        false,
        NULL, NULL, NULL
    },
#ifdef _PG_ORCL_
    {
        {"enable_oracle_compatible", PGC_USERSET, CUSTOM_OPTIONS,
            gettext_noop("Enable oracle compatibility."),
            NULL
        },
        &enable_oracle_compatible,
        false,
        NULL, NULL, NULL
    },
#endif

#ifdef __SUPPORT_DISTRIBUTED_TRANSACTION__
    {
        {"support_oracle_compatible", PGC_POSTMASTER, COMPAT_OPTIONS,
            gettext_noop("Support oracle compatibility."),
            NULL
        },
        &support_oracle_compatible,
        true,
        NULL, NULL, NULL
    },
#endif

    {
        {"logging_collector", PGC_POSTMASTER, LOGGING_WHERE,
            gettext_noop("Start a subprocess to capture stderr output and/or csvlogs into log files."),
            NULL
        },
        &Logging_collector,
        false,
        NULL, NULL, NULL
    },
    {
        {"log_truncate_on_rotation", PGC_SIGHUP, LOGGING_WHERE,
            gettext_noop("Truncate existing log files of same name during log rotation."),
            NULL
        },
        &Log_truncate_on_rotation,
        false,
        NULL, NULL, NULL
    },
#ifdef __AUDIT__
    {
        {"alog_truncate_on_rotation", PGC_SIGHUP, LOGGING_WHERE,
            gettext_noop("Truncate existing log files of same name during log rotation."),
            NULL
        },
        &AuditLog_truncate_on_rotation,
        false,
        NULL, NULL, NULL
    },
#endif

#ifdef TRACE_SORT
    {
        {"trace_sort", PGC_USERSET, DEVELOPER_OPTIONS,
            gettext_noop("Emit information about resource usage in sorting."),
            NULL,
            GUC_NOT_IN_SAMPLE
        },
        &trace_sort,
        false,
        NULL, NULL, NULL
    },
#endif

#ifdef TRACE_SYNCSCAN
    /* this is undocumented because not exposed in a standard build */
    {
        {"trace_syncscan", PGC_USERSET, DEVELOPER_OPTIONS,
            gettext_noop("Generate debugging output for synchronized scanning."),
            NULL,
            GUC_NOT_IN_SAMPLE
        },
        &trace_syncscan,
        false,
        NULL, NULL, NULL
    },
#endif

#ifdef DEBUG_BOUNDED_SORT
    /* this is undocumented because not exposed in a standard build */
    {
        {
            "optimize_bounded_sort", PGC_USERSET, QUERY_TUNING_METHOD,
            gettext_noop("Enable bounded sorting using heap sort."),
            NULL,
            GUC_NOT_IN_SAMPLE
        },
        &optimize_bounded_sort,
        true,
        NULL, NULL, NULL
    },
#endif

#ifdef WAL_DEBUG
    {
        {"wal_debug", PGC_SUSET, DEVELOPER_OPTIONS,
            gettext_noop("Emit WAL-related debugging output."),
            NULL,
            GUC_NOT_IN_SAMPLE
        },
        &XLOG_DEBUG,
        false,
        NULL, NULL, NULL
    },
#endif

    {
        {"integer_datetimes", PGC_INTERNAL, PRESET_OPTIONS,
            gettext_noop("Datetimes are integer based."),
            NULL,
            GUC_REPORT | GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
        },
        &integer_datetimes,
        true,
        NULL, NULL, NULL
    },

    {
        {"krb_caseins_users", PGC_SIGHUP, CONN_AUTH_SECURITY,
            gettext_noop("Sets whether Kerberos and GSSAPI user names should be treated as case-insensitive."),
            NULL
        },
        &pg_krb_caseins_users,
        false,
        NULL, NULL, NULL
    },

    {
        {"escape_string_warning", PGC_USERSET, COMPAT_OPTIONS_PREVIOUS,
            gettext_noop("Warn about backslash escapes in ordinary string literals."),
            NULL
        },
        &escape_string_warning,
        true,
        NULL, NULL, NULL
    },

    {
        {"standard_conforming_strings", PGC_USERSET, COMPAT_OPTIONS_PREVIOUS,
            gettext_noop("Causes '...' strings to treat backslashes literally."),
            NULL,
            GUC_REPORT
        },
        &standard_conforming_strings,
        true,
        NULL, NULL, NULL
    },

    {
        {"synchronize_seqscans", PGC_USERSET, COMPAT_OPTIONS_PREVIOUS,
            gettext_noop("Enable synchronized sequential scans."),
            NULL
        },
        &synchronize_seqscans,
        true,
        NULL, NULL, NULL
    },

    {
        {"hot_standby", PGC_POSTMASTER, REPLICATION_STANDBY,
            gettext_noop("Allows connections and queries during recovery."),
            NULL
        },
        &EnableHotStandby,
        true,
        NULL, NULL, NULL
    },

    {
        {"hot_standby_feedback", PGC_SIGHUP, REPLICATION_STANDBY,
            gettext_noop("Allows feedback from a hot standby to the primary that will avoid query conflicts."),
            NULL
        },
        &hot_standby_feedback,
        false,
        NULL, NULL, NULL
    },

    {
        {"allow_system_table_mods", PGC_POSTMASTER, DEVELOPER_OPTIONS,
            gettext_noop("Allows modifications of the structure of system tables."),
            NULL,
            GUC_NOT_IN_SAMPLE
        },
        &allowSystemTableMods,
        false,
        NULL, NULL, NULL
    },

    {
        {"ignore_system_indexes", PGC_BACKEND, DEVELOPER_OPTIONS,
            gettext_noop("Disables reading from system indexes."),
            gettext_noop("It does not prevent updating the indexes, so it is safe "
                         "to use.  The worst consequence is slowness."),
            GUC_NOT_IN_SAMPLE
        },
        &IgnoreSystemIndexes,
        false,
        NULL, NULL, NULL
    },
#ifdef PGXC
    {
        {"persistent_datanode_connections", PGC_BACKEND, DEVELOPER_OPTIONS,
            gettext_noop("Session never releases acquired connections."),
            NULL,
            GUC_NOT_IN_SAMPLE
        },
        &PersistentConnections,
        false,
        check_persistent_connections, NULL, NULL
    },
    {
        {"xc_maintenance_mode", PGC_SUSET, XC_HOUSEKEEPING_OPTIONS,
            gettext_noop("Turn on XC maintenance mode."),
             gettext_noop("Can set ON by SET command by superuser.")
        },
        &xc_maintenance_mode,
        false,
        check_pgxc_maintenance_mode, NULL, NULL
    },
#endif

    {
        {"lo_compat_privileges", PGC_SUSET, COMPAT_OPTIONS_PREVIOUS,
            gettext_noop("Enables backward compatibility mode for privilege checks on large objects."),
            gettext_noop("Skips privilege checks when reading or modifying large objects, "
                         "for compatibility with PostgreSQL releases prior to 9.0.")
        },
        &lo_compat_privileges,
        false,
        NULL, NULL, NULL
    },

    {
        {"operator_precedence_warning", PGC_USERSET, COMPAT_OPTIONS_PREVIOUS,
            gettext_noop("Emit a warning for constructs that changed meaning since PostgreSQL 9.4."),
            NULL,
        },
        &operator_precedence_warning,
        false,
        NULL, NULL, NULL
    },

    {
        {"quote_all_identifiers", PGC_USERSET, COMPAT_OPTIONS_PREVIOUS,
            gettext_noop("When generating SQL fragments, quote all identifiers."),
            NULL,
        },
        &quote_all_identifiers,
        false,
        NULL, NULL, NULL
    },

    {
        {"data_checksums", PGC_INTERNAL, PRESET_OPTIONS,
            gettext_noop("Shows whether data checksums are turned on for this cluster."),
            NULL,
            GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
        },
        &data_checksums,
        false,
        NULL, NULL, NULL
    },

    {
        {"syslog_sequence_numbers", PGC_SIGHUP, LOGGING_WHERE,
            gettext_noop("Add sequence number to syslog messages to avoid duplicate suppression."),
            NULL
        },
        &syslog_sequence_numbers,
        true,
        NULL, NULL, NULL
    },

    {
        {"syslog_split_messages", PGC_SIGHUP, LOGGING_WHERE,
            gettext_noop("Split messages sent to syslog by lines and to fit into 1024 bytes."),
            NULL
        },
        &syslog_split_messages,
        true,
        NULL, NULL, NULL
    },

#ifdef __TBASE__
	{
		{"enable_statistic", PGC_SIGHUP, STATS_COLLECTOR,
			gettext_noop("collect statistic information for debug."),
			NULL
		},
		&enable_statistic,
		false,
		NULL, NULL, NULL
	},

    {
        {"debug_data_pump", PGC_SIGHUP, CUSTOM_OPTIONS,
            gettext_noop("enable debug to trace data pump."),
            NULL
        },
        &g_DataPumpDebug,
        false,
        NULL, NULL, NULL
    },

    {
        {"enable_pullup_subquery", PGC_USERSET, CUSTOM_OPTIONS,
            gettext_noop("pullup subquery to make execution more efficient."),
            NULL
        },
        &enable_pullup_subquery,
#ifdef _PG_REGRESS_
        true,
#else
        false,
#endif
        NULL, NULL, NULL
    },

    {
        {
            "enable_sampling_analyze", PGC_USERSET, CUSTOM_OPTIONS,
            gettext_noop("sampling rows from datanodes when doing analyze on coordinator."),
            NULL
        },
        &enable_sampling_analyze,
        true,
        NULL, NULL, NULL
    },

	{
		{
			"enable_pgbouncer", PGC_SIGHUP, STATS_COLLECTOR,
			gettext_noop("use pgbouncer as coordinator connection pool."),
			NULL
		},
		&g_enable_bouncer,
		false,
		NULL, NULL, NULL
	},   

    {
        {
            "enable_gtm_proxy", PGC_POSTMASTER, CUSTOM_OPTIONS,
            gettext_noop("Enable gtm proxy a child process of Postmaster."),
            NULL
        },
        &g_enable_gtm_proxy,
        false,
        NULL, NULL, NULL
    },

    {
        {"vacuum_debug_print", PGC_POSTMASTER, CUSTOM_OPTIONS,
            gettext_noop("vacuum debug print."),
            NULL
        },
        &vacuum_debug_print,
        false,
        NULL, NULL, NULL
    },

    {
        {"enable_multi_cluster", PGC_POSTMASTER, CUSTOM_OPTIONS,
            gettext_noop("Enable multiple clusters."),
            NULL
        },
        &enable_multi_cluster,
        true,
        NULL, NULL, NULL
    },

    {
        {"enable_multi_cluster_print", PGC_USERSET, CUSTOM_OPTIONS,
            gettext_noop("Enable multiple cluster print."),
            NULL
        },
        &enable_multi_cluster_print,
        false,
        NULL, NULL, NULL
    },

    {
        {"prefer_olap", PGC_USERSET, CUSTOM_OPTIONS,
            gettext_noop("Prefer to run OLAP."),
            NULL
        },
        &prefer_olap,
        true,
        NULL, NULL, NULL
    },

    {
        {"olap_optimizer", PGC_USERSET, CUSTOM_OPTIONS,
            gettext_noop("enable OLAP optimizer to make Query more efficient."),
            NULL
        },
        &olap_optimizer,
        true,
        NULL, NULL, NULL
    },    

    {
        {"enable_concurrently_index", PGC_USERSET, CUSTOM_OPTIONS,
            gettext_noop("enable create index concurrently."),
            NULL
        },
        &g_concurrently_index,
#ifdef _PG_REGRESS_
        false,
#else
        true,
#endif
        NULL, NULL, NULL
    },    

    {
        {"param_pass_down", PGC_USERSET, CUSTOM_OPTIONS,
            gettext_noop("enable exec param to pass down."),
            NULL
        },
        &paramPassDown,
        false,
        NULL, NULL, NULL
    },

    {
		{"enable_distributed_unique_plan", PGC_USERSET, CUSTOM_OPTIONS,
			gettext_noop("enable distributed unique plan."),
			NULL
		},
		&enable_distributed_unique_plan,
		true,
		NULL, NULL, NULL
	},

    {
        {"enable_null_string", PGC_USERSET, CUSTOM_OPTIONS,
            gettext_noop("enable nulls in string."),
            NULL
        },
        &enable_null_string,
        false,
        NULL, NULL, NULL
    },

    {
        {"distributed_query_analyze", PGC_USERSET, STATS_COLLECTOR,
            gettext_noop("enable collecting distributed query info."),
            NULL
        },
        &distributed_query_analyze,
        false,
        NULL, NULL, NULL
    },

    {
        {"wal_check", PGC_USERSET, CUSTOM_OPTIONS,
            gettext_noop("check consistency of wal."),
            NULL
        },
        &g_wal_check,
#ifdef _PG_REGRESS_
        true,
#else
        false,
#endif
        NULL, NULL, NULL
    },

    {
        {"transform_insert_to_copy", PGC_USERSET, CUSTOM_OPTIONS,
            gettext_noop("try to transform insert into multi-values to copy from."),
            NULL
        },
        &g_transform_insert_to_copy,
#ifdef _PG_REGRESS_
        false,
#else
        true,
#endif
        NULL, NULL, NULL
    },

    {
        {"set_global_snapshot", PGC_USERSET, CUSTOM_OPTIONS,
            gettext_noop("always use global snapshot for query"),
            NULL
        },
        &g_set_global_snapshot,
        true,
        NULL, NULL, NULL
    },

    {
        {"restrict_query", PGC_USERSET, CUSTOM_OPTIONS,
            gettext_noop("restrict query to involved node as possible"),
            NULL
        },
        &restrict_query,
        true,
        NULL, NULL, NULL
    },

	{
		{"hybrid_hash_agg", PGC_USERSET, CUSTOM_OPTIONS,
			gettext_noop("enable hybrid-hash agg."),
			NULL
		},
		&g_hybrid_hash_agg,
		false,
		NULL, NULL, NULL
	},	

	{
		{"hybrid_hash_agg_debug", PGC_USERSET, CUSTOM_OPTIONS,
			gettext_noop("enable hybrid-hash agg debug."),
			NULL
		},
		&g_hybrid_hash_agg_debug,
		false,
		NULL, NULL, NULL
	},

	{
		{"enable_subquery_shipping", PGC_USERSET, CUSTOM_OPTIONS,
			gettext_noop("support fast query shipping for subquery"),
			NULL
		},
		&enable_subquery_shipping,
		true,
		NULL, NULL, NULL
	},
#endif

#ifdef _MIGRATE_
    {
        {"node_is_extension", PGC_POSTMASTER, CUSTOM_OPTIONS,
            gettext_noop("if this datanode is extension"),
            NULL
        },
        &g_IsExtension,
        false,
        NULL, NULL, NULL
    },
#endif
#ifdef __COLD_HOT__
    {
        {"enable_key_value", PGC_USERSET, PRESET_OPTIONS,
            gettext_noop("Enable key value lookup when make route strategy."),
            NULL
        },
        &g_EnableKeyValue,
        false,
        NULL, NULL, NULL
    },    

    {
        {"enable_cold_seperation", PGC_USERSET, PRESET_OPTIONS,
            gettext_noop("Enable cold storage seperation when make route strategy."),
            NULL
        },
        &g_EnableDualWrite,
        false,
        NULL, NULL, NULL
    },    

    {
        {"enable_cold_hot_visible", PGC_USERSET, PRESET_OPTIONS,
            gettext_noop("Enable cold-hot visible no matter on cold/hot datanode."),
            NULL
        },
        &g_EnableColdHotVisible,
        false,
        NULL, NULL, NULL
    },
#endif
#ifdef _SHARDING_
    {
        {"allow_dml_on_datanode", PGC_USERSET, CUSTOM_OPTIONS,
            gettext_noop("allow insert/update/delete directly on datanode"),
            NULL
        },
        &g_allow_dml_on_datanode,
        false,
        NULL, NULL, NULL
    },
	{
		{"allow_force_ddl", PGC_USERSET, CUSTOM_OPTIONS,
			gettext_noop("allow forced ddl of inconsistent metadata"),
			NULL
		},
		&g_allow_force_ddl,
		false,
		NULL, NULL, NULL
	},
    {
        {"trace_extent", PGC_SUSET, DEVELOPER_OPTIONS,
            gettext_noop("Emits information about extent changing."),
            NULL,
            GUC_NOT_IN_SAMPLE
        },
        &trace_extent,
        false,
        NULL, NULL, NULL
    },
    {
        {"enable_shard_statistic", PGC_SIGHUP, STATS_COLLECTOR,
            gettext_noop("collect statistic information for shard."),
            NULL
        },
        &g_StatShardInfo,
        true,
        NULL, NULL, NULL
    },
    {
        {"show_all_shard_stat", PGC_SUSET, STATS_COLLECTOR,
            gettext_noop("show statistic information for all shards."),
            NULL
        },
        &show_all_shard_stat,
        false,
        NULL, NULL, NULL
    },
#endif

#ifdef __AUDIT_FGA__
    {
        {"enable_fga", PGC_SIGHUP, CUSTOM_OPTIONS,
            gettext_noop("Enable Fine-grained audit."),
            NULL
        },
        &enable_fga,
#ifdef _PG_REGRESS_
        true,
#else
        false,
#endif
        NULL, NULL, NULL
    },
#endif

#ifdef __TBASE__
    {
        {"enable_pooler_debug_print", PGC_SUSET, CUSTOM_OPTIONS,
            gettext_noop("enable pooler manager debug infomation print"),
            NULL
        },
        &PoolConnectDebugPrint,
        false,
        NULL, NULL, NULL
    },
    {
        {"enable_pooler_thread_log_print", PGC_USERSET, CUSTOM_OPTIONS,
         gettext_noop("enable pooler manager sub thread log print"),
         NULL
        },
        &PoolSubThreadLogPrint,
        true,
        NULL, NULL, NULL
    },
	{
        {"enable_plpgsql_debug_print", PGC_SUSET, CUSTOM_OPTIONS,
            gettext_noop("enable plpgsql debug infomation print"),
            NULL
        },
        &PlpgsqlDebugPrint,
        false,
        NULL, NULL, NULL
    },

    {
        {"enable_gtm_debug_print", PGC_SUSET, CUSTOM_OPTIONS,
            gettext_noop("enable gtm debug infomation print"),
            NULL
        },
        &GTMDebugPrint,
        false,
        NULL, NULL, NULL
    },
    {
        {"skip_gtm_catalog", PGC_POSTMASTER, CUSTOM_OPTIONS,
            gettext_noop("used to skip gtm catalog, WARNING:only for emergency purpose and only avaliable on coordinators."),
            NULL
        },
        &g_GTM_skip_catalog,
        false,
        NULL, NULL, NULL
    },

    {
        {"enable_distri_debug_print", PGC_SUSET, CUSTOM_OPTIONS,
            gettext_noop("enable distributed transaction debug print"),
            NULL
        },
        &enable_distri_print,
        false,
        NULL, NULL, NULL
    },

    {
        {"enable_distri_visibility_print", PGC_SUSET, CUSTOM_OPTIONS,
            gettext_noop("enable distributed transaction visibility print"),
            NULL
        },
        &enable_distri_visibility_print,
        false,
        NULL, NULL, NULL
    },
        
    {
        {"enable_distri_debug", PGC_SUSET, CUSTOM_OPTIONS,
            gettext_noop("enable distributed transaction debug"),
            NULL
        },
        &enable_distri_debug,
        false,
        NULL, NULL, NULL
    },

    

    {
        {"enable_committs_print", PGC_SUSET, CUSTOM_OPTIONS,
            gettext_noop("enable commit ts debug print"),
            NULL
        },
        &enable_committs_print,
        false,
        NULL, NULL, NULL
    },


    {
        {"enable_pooler_stuck_exit", PGC_SIGHUP, CUSTOM_OPTIONS,
            gettext_noop("enable pooler exit when pick up sync network thread failed"),
            NULL
        },
        &PoolerStuckExit,
        false,
        NULL, NULL, NULL
    },
    
    {
        {"warm_shared_buffer", PGC_USERSET, CLIENT_CONN_STATEMENT,
            gettext_noop("Set connection to warm shared buffer."),
            NULL,
            GUC_NO_RESET_ALL | GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
        },
        &g_WarmSharedBuffer,
        false,
        set_warm_shared_buffer, NULL, NULL
    },    
    {
        {"enable_copy_silence", PGC_USERSET, PRESET_OPTIONS,
            gettext_noop("Enable copy from silent when error happens."),
            NULL
        },
        &g_enable_copy_silence,
        false,
        NULL, NULL, NULL
    },
    {
        {"enable_user_authority_force_check", PGC_POSTMASTER, CUSTOM_OPTIONS,
            gettext_noop("control users to get the list of tables and functions which can be accessed and executed by these user."),
            NULL
        },
        &g_enable_user_authority_force_check,
        false,
        NULL, NULL, NULL
    },
#endif

#ifdef __AUDIT__
    {
        {"enable_audit", PGC_SIGHUP, CUSTOM_OPTIONS,
            gettext_noop("Enable to audit user operations on the database objects."),
            NULL
        },
        &enable_audit,
        false,
        NULL, NULL, NULL
    },
    {
        {"enable_audit_warning", PGC_USERSET, CUSTOM_OPTIONS,
            gettext_noop("Enable to print audit warning logs."),
            NULL
        },
        &enable_audit_warning,
        false,
        NULL, NULL, NULL
    },
    {
        {"enable_auditlogger_warning", PGC_USERSET, CUSTOM_OPTIONS,
            gettext_noop("Enable to write audit logger process warnings."),
            NULL
        },
        &enable_auditlogger_warning,
        false,
        NULL, NULL, NULL
    },
#endif
#ifdef _MLS_
    {
        {"enable_cls", PGC_POSTMASTER, CUSTOM_OPTIONS,
            gettext_noop("Enable to Cube Lable Security check."),
            NULL
        },
        &g_enable_cls,
#ifdef _PG_REGRESS_
        true,
#else
        false,
#endif
        NULL, NULL, NULL
    },
    {
        {"enable_data_mask", PGC_POSTMASTER, CUSTOM_OPTIONS,
            gettext_noop("Enable to datamask feature."),
            NULL
        },
        &g_enable_data_mask,
        true,
        NULL, NULL, NULL
    },
    {
        {"enable_transparent_crypt", PGC_POSTMASTER, CUSTOM_OPTIONS,
            gettext_noop("Enable to transparent crypt feature."),
            NULL
        },
        &g_enable_transparent_crypt,
        true,
        NULL, NULL, NULL
    },
    {
        {"enable_crypt_debug", PGC_POSTMASTER, CUSTOM_OPTIONS,
            gettext_noop("Enable debug for crypt feature."),
            NULL
        },
        &g_enable_crypt_debug,
#ifdef _PG_REGRESS_
        true,
#else
        false,
#endif
        NULL, NULL, NULL
    },
    {
        {"enable_check_password", PGC_SUSET, CUSTOM_OPTIONS,
            gettext_noop("Enable password check."),
            gettext_noop("A valid password should contain upper case letter, lower case letter, number and special character. "
               "The length of password should greater or equal than 8. ")
        },
        &g_CheckPassword,
        false,
        NULL, NULL, NULL
    },
    {
        {"enable_crypt_parellel_debug", PGC_SUSET, CUSTOM_OPTIONS,
            gettext_noop("Enable trace buffer crypt procedure."),
            NULL
        },
        &g_enable_crypt_parellel_debug,
        false,
        NULL, NULL, NULL
    },
    {
        {"enable_crypt_check", PGC_POSTMASTER, CUSTOM_OPTIONS,
            gettext_noop("Enable check crypt consistency, the crypted context could be decrypted, and the value is the same as original."),
            NULL
        },
        &g_enable_crypt_check,
        false,
        NULL, NULL, NULL
    },
#endif
#ifdef XCP
    {
        {"random_collect_stats", PGC_USERSET, STATS,
            gettext_noop("Coordinator random collects the statistics from all data nodes."),
            NULL
        },
        &random_collect_stats,
        true,
        NULL, NULL, NULL
    },
#endif
#ifdef _PUB_SUB_RELIABLE_
    {
        {"enable_replication_slot_debug", PGC_USERSET, STATS,
            gettext_noop("enable_replication_slot_debug"),
            NULL
        },
        &g_replication_slot_debug,
        false,
        NULL, NULL, NULL
    },
#endif

#ifdef __TBASE__
	{
		{"enable_lock_account", PGC_SUSET, CUSTOM_OPTIONS,
			gettext_noop("Enable lock account when login fail serval times."),
			NULL
		},
		&enable_lock_account,
		false,
		NULL, NULL, NULL
	},
	{
		{"lock_account_print", PGC_SUSET, CUSTOM_OPTIONS,
			gettext_noop("Enable print log in lock account procedure."),
			NULL
		},
		&lock_account_print,
		false,
		NULL, NULL, NULL
	},
    {
        {"enable_parallel_ddl", PGC_USERSET, CUSTOM_OPTIONS,
             gettext_noop("Enable parallel DDL with no deadlock."),
             NULL
        },
        &enable_parallel_ddl,
        false,
        NULL, NULL, NULL
    },
    {
		{"enable_buffer_mprotect", PGC_POSTMASTER, CUSTOM_OPTIONS,
			gettext_noop("Protect memory corruption for share buffer"),
			NULL,
			GUC_NOT_IN_SAMPLE,
		},
		&enable_buffer_mprotect,
#ifdef _PG_REGRESS_
		true,
#else
		false,
#endif
		NULL, NULL, NULL
	},
	{
		{"enable_clog_mprotect", PGC_POSTMASTER, CUSTOM_OPTIONS,
			gettext_noop("Protect memory corruption for clog"),
			NULL,
			GUC_NOT_IN_SAMPLE,
		},
		&enable_clog_mprotect,
#ifdef _PG_REGRESS_
		true,
#else
		false,
#endif
		NULL, NULL, NULL
	},
	{
	{"enable_tlog_mprotect", PGC_POSTMASTER, CUSTOM_OPTIONS,
		gettext_noop("Protect memory corruption for tlog"),
		NULL,
		GUC_NOT_IN_SAMPLE,
		},
		&enable_tlog_mprotect,
#ifdef _PG_REGRESS_
		true,
#else
		false,
#endif
		NULL, NULL, NULL
	},
	{
		{"enable_xlog_mprotect", PGC_POSTMASTER, CUSTOM_OPTIONS,
			gettext_noop("Protect memory corruption for xlog"),
			NULL,
			GUC_NOT_IN_SAMPLE,
		},
		&enable_xlog_mprotect,
#ifdef _PG_REGRESS_
		true,
#else
		false,
#endif
		NULL, NULL, NULL
	},
	{
		{"enable_cold_hot_router_print", PGC_USERSET, CUSTOM_OPTIONS,
			 gettext_noop("Whether print cold hot router."),
			 NULL
		},
		&enable_cold_hot_router_print,
		false,
		NULL, NULL, NULL
	},
    {
		{"enable_memory_optimization", PGC_POSTMASTER, RESOURCES,
			gettext_noop("enable session cache memory control"),
			NULL
		},
		&enable_memory_optimization,
#ifdef _PG_REGRESS_
		true,
#else
		false,
#endif
		NULL, NULL, NULL
	},

#endif
	{
		{"enable_distinct_optimizer", PGC_SUSET, CUSTOM_OPTIONS,
			 gettext_noop("push down distinct to datanodes."),
			 NULL
		},
		&enable_distinct_optimizer
		,
#ifdef _PG_REGRESS_
		true,
#else
		false,
#endif
		NULL, NULL, NULL
	},

    /* End-of-list marker */
    {
        {NULL, 0, 0, NULL, NULL}, NULL, false, NULL, NULL, NULL
    }
};


static struct config_int ConfigureNamesInt[] =
{
    {
        {"archive_timeout", PGC_SIGHUP, WAL_ARCHIVING,
            gettext_noop("Forces a switch to the next WAL file if a "
                         "new file has not been started within N seconds."),
            NULL,
            GUC_UNIT_S
        },
        &XLogArchiveTimeout,
        0, 0, INT_MAX / 2,
        NULL, NULL, NULL
    },
    {
        {"post_auth_delay", PGC_BACKEND, DEVELOPER_OPTIONS,
            gettext_noop("Waits N seconds on connection startup after authentication."),
            gettext_noop("This allows attaching a debugger to the process."),
            GUC_NOT_IN_SAMPLE | GUC_UNIT_S
        },
        &PostAuthDelay,
        0, 0, INT_MAX / 1000000,
        NULL, NULL, NULL
    },
    {
        {"default_statistics_target", PGC_USERSET, QUERY_TUNING_OTHER,
            gettext_noop("Sets the default statistics target."),
            gettext_noop("This applies to table columns that have not had a "
                         "column-specific target set via ALTER TABLE SET STATISTICS.")
        },
        &default_statistics_target,
        100, 1, 10000,
        NULL, NULL, NULL
    },
    {
        {"from_collapse_limit", PGC_USERSET, QUERY_TUNING_OTHER,
            gettext_noop("Sets the FROM-list size beyond which subqueries "
                         "are not collapsed."),
            gettext_noop("The planner will merge subqueries into upper "
                         "queries if the resulting FROM list would have no more than "
                         "this many items.")
        },
        &from_collapse_limit,
        8, 1, INT_MAX,
        NULL, NULL, NULL
    },
    {
        {"join_collapse_limit", PGC_USERSET, QUERY_TUNING_OTHER,
            gettext_noop("Sets the FROM-list size beyond which JOIN "
                         "constructs are not flattened."),
            gettext_noop("The planner will flatten explicit JOIN "
                         "constructs into lists of FROM items whenever a "
                         "list of no more than this many items would result.")
        },
        &join_collapse_limit,
        8, 1, INT_MAX,
        NULL, NULL, NULL
    },
    {
        {"geqo_threshold", PGC_USERSET, QUERY_TUNING_GEQO,
            gettext_noop("Sets the threshold of FROM items beyond which GEQO is used."),
            NULL
        },
        &geqo_threshold,
        12, 2, INT_MAX,
        NULL, NULL, NULL
    },
    {
        {"geqo_effort", PGC_USERSET, QUERY_TUNING_GEQO,
            gettext_noop("GEQO: effort is used to set the default for other GEQO parameters."),
            NULL
        },
        &Geqo_effort,
        DEFAULT_GEQO_EFFORT, MIN_GEQO_EFFORT, MAX_GEQO_EFFORT,
        NULL, NULL, NULL
    },
    {
        {"geqo_pool_size", PGC_USERSET, QUERY_TUNING_GEQO,
            gettext_noop("GEQO: number of individuals in the population."),
            gettext_noop("Zero selects a suitable default value.")
        },
        &Geqo_pool_size,
        0, 0, INT_MAX,
        NULL, NULL, NULL
    },
    {
        {"geqo_generations", PGC_USERSET, QUERY_TUNING_GEQO,
            gettext_noop("GEQO: number of iterations of the algorithm."),
            gettext_noop("Zero selects a suitable default value.")
        },
        &Geqo_generations,
        0, 0, INT_MAX,
        NULL, NULL, NULL
    },

    {
        /* This is PGC_SUSET to prevent hiding from log_lock_waits. */
        {"deadlock_timeout", PGC_SUSET, LOCK_MANAGEMENT,
            gettext_noop("Sets the time to wait on a lock before checking for deadlock."),
            NULL,
            GUC_UNIT_MS
        },
        &DeadlockTimeout,
        1000, 1, INT_MAX,
        NULL, NULL, NULL
    },

    {
        {"max_standby_archive_delay", PGC_SIGHUP, REPLICATION_STANDBY,
            gettext_noop("Sets the maximum delay before canceling queries when a hot standby server is processing archived WAL data."),
            NULL,
            GUC_UNIT_MS
        },
        &max_standby_archive_delay,
        30 * 1000, -1, INT_MAX,
        NULL, NULL, NULL
    },

    {
        {"max_standby_streaming_delay", PGC_SIGHUP, REPLICATION_STANDBY,
            gettext_noop("Sets the maximum delay before canceling queries when a hot standby server is processing streamed WAL data."),
            NULL,
            GUC_UNIT_MS
        },
        &max_standby_streaming_delay,
        30 * 1000, -1, INT_MAX,
        NULL, NULL, NULL
    },

    {
        {"wal_receiver_status_interval", PGC_SIGHUP, REPLICATION_STANDBY,
            gettext_noop("Sets the maximum interval between WAL receiver status reports to the primary."),
            NULL,
            GUC_UNIT_S
        },
        &wal_receiver_status_interval,
        10, 0, INT_MAX / 1000,
        NULL, NULL, NULL
    },

    {
        {"wal_receiver_timeout", PGC_SIGHUP, REPLICATION_STANDBY,
            gettext_noop("Sets the maximum wait time to receive data from the primary."),
            NULL,
            GUC_UNIT_MS
        },
        &wal_receiver_timeout,
        60 * 1000, 0, INT_MAX,
        NULL, NULL, NULL
    },

    {
        {"max_connections", PGC_POSTMASTER, CONN_AUTH_SETTINGS,
            gettext_noop("Sets the maximum number of concurrent connections."),
            NULL
        },
        &MaxConnections,
        100, 1, MAX_BACKENDS,
        check_maxconnections, NULL, NULL
    },

    {
        {"superuser_reserved_connections", PGC_POSTMASTER, CONN_AUTH_SETTINGS,
            gettext_noop("Sets the number of connection slots reserved for superusers."),
            NULL
        },
        &ReservedBackends,
        3, 0, MAX_BACKENDS,
        NULL, NULL, NULL
    },

    /*
     * We sometimes multiply the number of shared buffers by two without
     * checking for overflow, so we mustn't allow more than INT_MAX / 2.
     */
    {
        {"shared_buffers", PGC_POSTMASTER, RESOURCES_MEM,
            gettext_noop("Sets the number of shared memory buffers used by the server."),
            NULL,
            GUC_UNIT_BLOCKS
        },
        &NBuffers,
        1024, 16, INT_MAX / 2,
        NULL, NULL, NULL
    },

    {
        {"temp_buffers", PGC_USERSET, RESOURCES_MEM,
            gettext_noop("Sets the maximum number of temporary buffers used by each session."),
            NULL,
            GUC_UNIT_BLOCKS
        },
        &num_temp_buffers,
        1024, 100, INT_MAX / 2,
        check_temp_buffers, NULL, NULL
    },

    {
        {"port", PGC_POSTMASTER, CONN_AUTH_SETTINGS,
            gettext_noop("Sets the TCP port the server listens on."),
            NULL
        },
        &PostPortNumber,
        DEF_PGPORT, 1, 65535,
        NULL, NULL, NULL
    },

    {
        {"unix_socket_permissions", PGC_POSTMASTER, CONN_AUTH_SETTINGS,
            gettext_noop("Sets the access permissions of the Unix-domain socket."),
            gettext_noop("Unix-domain sockets use the usual Unix file system "
                         "permission set. The parameter value is expected "
                         "to be a numeric mode specification in the form "
                         "accepted by the chmod and umask system calls. "
                         "(To use the customary octal format the number must "
                         "start with a 0 (zero).)")
        },
        &Unix_socket_permissions,
        0777, 0000, 0777,
        NULL, NULL, show_unix_socket_permissions
    },

    {
        {"log_file_mode", PGC_SIGHUP, LOGGING_WHERE,
            gettext_noop("Sets the file permissions for log files."),
            gettext_noop("The parameter value is expected "
                         "to be a numeric mode specification in the form "
                         "accepted by the chmod and umask system calls. "
                         "(To use the customary octal format the number must "
                         "start with a 0 (zero).)")
        },
        &Log_file_mode,
        0600, 0000, 0777,
        NULL, NULL, show_log_file_mode
    },
#ifdef __AUDIT__
    {
        {"alog_file_mode", PGC_SIGHUP, LOGGING_WHERE,
            gettext_noop("Sets the file permissions for audit log files."),
            gettext_noop("The parameter value is expected "
                         "to be a numeric mode specification in the form "
                         "accepted by the chmod and umask system calls. "
                         "(To use the customary octal format the number must "
                         "start with a 0 (zero).)")
        },
        &AuditLog_file_mode,
        0600, 0000, 0777,
        NULL, NULL, show_alog_file_mode
    },
#endif
    {
        {"work_mem", PGC_USERSET, RESOURCES_MEM,
            gettext_noop("Sets the maximum memory to be used for query workspaces."),
            gettext_noop("This much memory can be used by each internal "
                         "sort operation and hash table before switching to "
                         "temporary disk files."),
            GUC_UNIT_KB
        },
        &work_mem,
        4096, 64, MAX_KILOBYTES,
        NULL, NULL, NULL
    },

    {
        {"maintenance_work_mem", PGC_USERSET, RESOURCES_MEM,
            gettext_noop("Sets the maximum memory to be used for maintenance operations."),
            gettext_noop("This includes operations such as VACUUM and CREATE INDEX."),
            GUC_UNIT_KB
        },
        &maintenance_work_mem,
        65536, 1024, MAX_KILOBYTES,
        NULL, NULL, NULL
    },

    {
        {"replacement_sort_tuples", PGC_USERSET, RESOURCES_MEM,
            gettext_noop("Sets the maximum number of tuples to be sorted using replacement selection."),
            gettext_noop("When more tuples than this are present, quicksort will be used.")
        },
        &replacement_sort_tuples,
        150000, 0, INT_MAX,
        NULL, NULL, NULL
    },

    /*
     * We use the hopefully-safely-small value of 100kB as the compiled-in
     * default for max_stack_depth.  InitializeGUCOptions will increase it if
     * possible, depending on the actual platform-specific stack limit.
     */
    {
        {"max_stack_depth", PGC_SUSET, RESOURCES_MEM,
            gettext_noop("Sets the maximum stack depth, in kilobytes."),
            NULL,
            GUC_UNIT_KB
        },
        &max_stack_depth,
        100, 100, MAX_KILOBYTES,
        check_max_stack_depth, assign_max_stack_depth, NULL
    },

    {
        {"temp_file_limit", PGC_SUSET, RESOURCES_DISK,
            gettext_noop("Limits the total size of all temporary files used by each process."),
            gettext_noop("-1 means no limit."),
            GUC_UNIT_KB
        },
        &temp_file_limit,
        -1, -1, INT_MAX,
        NULL, NULL, NULL
    },

    {
        {"vacuum_cost_page_hit", PGC_USERSET, RESOURCES_VACUUM_DELAY,
            gettext_noop("Vacuum cost for a page found in the buffer cache."),
            NULL
        },
        &VacuumCostPageHit,
        1, 0, 10000,
        NULL, NULL, NULL
    },

    {
        {"vacuum_cost_page_miss", PGC_USERSET, RESOURCES_VACUUM_DELAY,
            gettext_noop("Vacuum cost for a page not found in the buffer cache."),
            NULL
        },
        &VacuumCostPageMiss,
        10, 0, 10000,
        NULL, NULL, NULL
    },

    {
        {"vacuum_cost_page_dirty", PGC_USERSET, RESOURCES_VACUUM_DELAY,
            gettext_noop("Vacuum cost for a page dirtied by vacuum."),
            NULL
        },
        &VacuumCostPageDirty,
        20, 0, 10000,
        NULL, NULL, NULL
    },

    {
        {"vacuum_cost_limit", PGC_USERSET, RESOURCES_VACUUM_DELAY,
            gettext_noop("Vacuum cost amount available before napping."),
            NULL
        },
        &VacuumCostLimit,
        200, 1, 10000,
        NULL, NULL, NULL
    },

    {
        {"vacuum_cost_delay", PGC_USERSET, RESOURCES_VACUUM_DELAY,
            gettext_noop("Vacuum cost delay in milliseconds."),
            NULL,
            GUC_UNIT_MS
        },
        &VacuumCostDelay,
        0, 0, 100,
        NULL, NULL, NULL
    },

    {
        {"autovacuum_vacuum_cost_delay", PGC_SIGHUP, AUTOVACUUM,
            gettext_noop("Vacuum cost delay in milliseconds, for autovacuum."),
            NULL,
            GUC_UNIT_MS
        },
        &autovacuum_vac_cost_delay,
        20, -1, 100,
        NULL, NULL, NULL
    },

    {
        {"autovacuum_vacuum_cost_limit", PGC_SIGHUP, AUTOVACUUM,
            gettext_noop("Vacuum cost amount available before napping, for autovacuum."),
            NULL
        },
        &autovacuum_vac_cost_limit,
        -1, -1, 10000,
        NULL, NULL, NULL
    },

	{
		{"gts_maintain_option", PGC_SIGHUP, DEVELOPER_OPTIONS,
			gettext_noop("Enables check correctness of GTS and reseting it if it is wrong"),
			NULL
		},
		&gts_maintain_option,
		0, 0, 2,
		NULL, NULL, NULL
	},

	{
		{"max_files_per_process", PGC_POSTMASTER, RESOURCES_KERNEL,
			gettext_noop("Sets the maximum number of simultaneously open files for each server process."),
			NULL
		},
		&max_files_per_process,
		1000, 25, INT_MAX,
		NULL, NULL, NULL
	},

    /*
     * See also CheckRequiredParameterValues() if this parameter changes
     */
    {
        {"max_prepared_transactions", PGC_POSTMASTER, RESOURCES_MEM,
            gettext_noop("Sets the maximum number of simultaneously prepared transactions."),
            NULL
        },
        &max_prepared_xacts,
#ifdef PGXC
        10000, 0, INT_MAX / 4,
        NULL, NULL, NULL
#else
        0, 0, MAX_BACKENDS,
        NULL, NULL, NULL
#endif
    },

#ifdef LOCK_DEBUG
    {
        {"trace_lock_oidmin", PGC_SUSET, DEVELOPER_OPTIONS,
            gettext_noop("Sets the minimum OID of tables for tracking locks."),
            gettext_noop("Is used to avoid output on system tables."),
            GUC_NOT_IN_SAMPLE
        },
        &Trace_lock_oidmin,
        FirstNormalObjectId, 0, INT_MAX,
        NULL, NULL, NULL
    },
    {
        {"trace_lock_table", PGC_SUSET, DEVELOPER_OPTIONS,
            gettext_noop("Sets the OID of the table with unconditionally lock tracing."),
            NULL,
            GUC_NOT_IN_SAMPLE
        },
        &Trace_lock_table,
        0, 0, INT_MAX,
        NULL, NULL, NULL
    },
#endif

    {
        {"statement_timeout", PGC_USERSET, CLIENT_CONN_STATEMENT,
            gettext_noop("Sets the maximum allowed duration of any statement."),
            gettext_noop("A value of 0 turns off the timeout."),
            GUC_UNIT_MS
        },
        &StatementTimeout,
        0, 0, INT_MAX,
        NULL, NULL, NULL
    },

    {
        {"lock_timeout", PGC_USERSET, CLIENT_CONN_STATEMENT,
            gettext_noop("Sets the maximum allowed duration of any wait for a lock."),
            gettext_noop("A value of 0 turns off the timeout."),
            GUC_UNIT_MS
        },
        &LockTimeout,
        0, 0, INT_MAX,
        NULL, NULL, NULL
    },

    {
        {"idle_in_transaction_session_timeout", PGC_USERSET, CLIENT_CONN_STATEMENT,
            gettext_noop("Sets the maximum allowed duration of any idling transaction."),
            gettext_noop("A value of 0 turns off the timeout."),
            GUC_UNIT_MS
        },
        &IdleInTransactionSessionTimeout,
        0, 0, INT_MAX,
        NULL, NULL, NULL
    },

    {
        {"vacuum_freeze_min_age", PGC_USERSET, CLIENT_CONN_STATEMENT,
            gettext_noop("Minimum age at which VACUUM should freeze a table row."),
            NULL
        },
        &vacuum_freeze_min_age,
        50000000, 0, 1000000000,
        NULL, NULL, NULL
    },

    {
        {"vacuum_defer_freeze_min_age", PGC_USERSET, CLIENT_CONN_STATEMENT,
            gettext_noop("Minimum age at which VACUUM should defer to freeze a table row to avoid failure due to "
                    "too old timestamp."),
            NULL
        },
        &vacuum_defer_freeze_min_age,
        10000, 0, 1000000000,
        NULL, NULL, NULL
    },

    {
        {"vacuum_freeze_table_age", PGC_USERSET, CLIENT_CONN_STATEMENT,
            gettext_noop("Age at which VACUUM should scan whole table to freeze tuples."),
            NULL
        },
        &vacuum_freeze_table_age,
        150000000, 0, 2000000000,
        NULL, NULL, NULL
    },

    {
        {"vacuum_multixact_freeze_min_age", PGC_USERSET, CLIENT_CONN_STATEMENT,
            gettext_noop("Minimum age at which VACUUM should freeze a MultiXactId in a table row."),
            NULL
        },
        &vacuum_multixact_freeze_min_age,
        5000000, 0, 1000000000,
        NULL, NULL, NULL
    },

    {
        {"vacuum_multixact_freeze_table_age", PGC_USERSET, CLIENT_CONN_STATEMENT,
            gettext_noop("Multixact age at which VACUUM should scan whole table to freeze tuples."),
            NULL
        },
        &vacuum_multixact_freeze_table_age,
        150000000, 0, 2000000000,
        NULL, NULL, NULL
    },

    {
        {"vacuum_defer_cleanup_age", PGC_SIGHUP, REPLICATION_MASTER,
            gettext_noop("Number of transactions by which VACUUM and HOT cleanup should be deferred, if any."),
            NULL
        },
        &vacuum_defer_cleanup_age,
        0, 0, 1000000,
        NULL, NULL, NULL
    },
                
    {
        {"vacuum_delta", PGC_POSTMASTER, REPLICATION_MASTER,
            gettext_noop("The time period before which VACUUM and HOT cleanup could clean the transactions, if any."),
            NULL
        },
        &vacuum_delta,
        100, 1, INT_MAX,
        NULL, NULL, NULL
    },
        
    {
        {"delay_before_acquire_committs", PGC_USERSET, DEVELOPER_OPTIONS,
            gettext_noop("Delay transaction commit before acquiring commmitts (us)."),
            NULL
        },
        &delay_before_acquire_committs,
        0, 0, 100000000,
        NULL, NULL, NULL
    },
    {
        {"delay_after_acquire_committs", PGC_USERSET, DEVELOPER_OPTIONS,
            gettext_noop("Delay transaction commit after acquiring commmitts (us)."),
            NULL
        },
        &delay_after_acquire_committs,
        0, 0, 100000000,
        NULL, NULL, NULL
    },


    {
        {"page_ts_need_xlog", PGC_SIGHUP, REPLICATION_MASTER,
            gettext_noop("Determine whether the page timestamp should be logged."),
            NULL
        },
        &page_ts_need_xlog,
        1, 0, 1,
        NULL, NULL, NULL
    },    
    /*
     * See also CheckRequiredParameterValues() if this parameter changes
     */
    {
        {"max_locks_per_transaction", PGC_POSTMASTER, LOCK_MANAGEMENT,
            gettext_noop("Sets the maximum number of locks per transaction."),
            gettext_noop("The shared lock table is sized on the assumption that "
                         "at most max_locks_per_transaction * max_connections distinct "
                         "objects will need to be locked at any one time.")
        },
        &max_locks_per_xact,
        64, 10, INT_MAX,
        NULL, NULL, NULL
    },

    {
        {"max_pred_locks_per_transaction", PGC_POSTMASTER, LOCK_MANAGEMENT,
            gettext_noop("Sets the maximum number of predicate locks per transaction."),
            gettext_noop("The shared predicate lock table is sized on the assumption that "
                         "at most max_pred_locks_per_transaction * max_connections distinct "
                         "objects will need to be locked at any one time.")
        },
        &max_predicate_locks_per_xact,
        64, 10, INT_MAX,
        NULL, NULL, NULL
    },

    {
        {"max_pred_locks_per_relation", PGC_SIGHUP, LOCK_MANAGEMENT,
            gettext_noop("Sets the maximum number of predicate-locked pages and tuples per relation."),
            gettext_noop("If more than this total of pages and tuples in the same relation are locked "
                         "by a connection, those locks are replaced by a relation level lock.")
        },
        &max_predicate_locks_per_relation,
        -2, INT_MIN, INT_MAX,
        NULL, NULL, NULL
    },

    {
        {"max_pred_locks_per_page", PGC_SIGHUP, LOCK_MANAGEMENT,
            gettext_noop("Sets the maximum number of predicate-locked tuples per page."),
            gettext_noop("If more than this number of tuples on the same page are locked "
                         "by a connection, those locks are replaced by a page level lock.")
        },
        &max_predicate_locks_per_page,
        2, 0, INT_MAX,
        NULL, NULL, NULL
    },

    {
        {"authentication_timeout", PGC_SIGHUP, CONN_AUTH_SECURITY,
            gettext_noop("Sets the maximum allowed time to complete client authentication."),
            NULL,
            GUC_UNIT_S
        },
        &AuthenticationTimeout,
        60, 1, 600,
        NULL, NULL, NULL
    },

    {
        /* Not for general use */
        {"pre_auth_delay", PGC_SIGHUP, DEVELOPER_OPTIONS,
            gettext_noop("Waits N seconds on connection startup before authentication."),
            gettext_noop("This allows attaching a debugger to the process."),
            GUC_NOT_IN_SAMPLE | GUC_UNIT_S
        },
        &PreAuthDelay,
        0, 0, 60,
        NULL, NULL, NULL
    },

    {
        {"wal_keep_segments", PGC_SIGHUP, REPLICATION_SENDING,
            gettext_noop("Sets the number of WAL files held for standby servers."),
            NULL
        },
        &wal_keep_segments,
        0, 0, INT_MAX,
        NULL, NULL, NULL
    },

    {
        {"min_wal_size", PGC_SIGHUP, WAL_CHECKPOINTS,
            gettext_noop("Sets the minimum size to shrink the WAL to."),
            NULL,
            GUC_UNIT_MB
        },
        &min_wal_size_mb,
        5 * (XLOG_SEG_SIZE / (1024 * 1024)), 2, MAX_KILOBYTES,
        NULL, NULL, NULL
    },

    {
        {"max_wal_size", PGC_SIGHUP, WAL_CHECKPOINTS,
            gettext_noop("Sets the WAL size that triggers a checkpoint."),
            NULL,
            GUC_UNIT_MB
        },
        &max_wal_size_mb,
        64 * (XLOG_SEG_SIZE / (1024 * 1024)), 2, MAX_KILOBYTES,
        NULL, assign_max_wal_size, NULL
    },

    {
        {"checkpoint_timeout", PGC_SIGHUP, WAL_CHECKPOINTS,
            gettext_noop("Sets the maximum time between automatic WAL checkpoints."),
            NULL,
            GUC_UNIT_S
        },
        &CheckPointTimeout,
        300, 30, 86400,
        NULL, NULL, NULL
    },

    {
        {"checkpoint_warning", PGC_SIGHUP, WAL_CHECKPOINTS,
            gettext_noop("Enables warnings if checkpoint segments are filled more "
                         "frequently than this."),
            gettext_noop("Write a message to the server log if checkpoints "
                         "caused by the filling of checkpoint segment files happens more "
                         "frequently than this number of seconds. Zero turns off the warning."),
            GUC_UNIT_S
        },
        &CheckPointWarning,
        30, 0, INT_MAX,
        NULL, NULL, NULL
    },

    {
        {"checkpoint_flush_after", PGC_SIGHUP, WAL_CHECKPOINTS,
            gettext_noop("Number of pages after which previously performed writes are flushed to disk."),
            NULL,
            GUC_UNIT_BLOCKS
        },
        &checkpoint_flush_after,
        DEFAULT_CHECKPOINT_FLUSH_AFTER, 0, WRITEBACK_MAX_PENDING_FLUSHES,
        NULL, NULL, NULL
    },

    {
        {"wal_buffers", PGC_POSTMASTER, WAL_SETTINGS,
            gettext_noop("Sets the number of disk-page buffers in shared memory for WAL."),
            NULL,
            GUC_UNIT_XBLOCKS
        },
        &XLOGbuffers,
        -1, -1, (INT_MAX / XLOG_BLCKSZ),
        check_wal_buffers, NULL, NULL
    },

    {
        {"wal_writer_delay", PGC_SIGHUP, WAL_SETTINGS,
            gettext_noop("Time between WAL flushes performed in the WAL writer."),
            NULL,
            GUC_UNIT_MS
        },
        &WalWriterDelay,
        200, 1, 10000,
        NULL, NULL, NULL
    },
#ifdef __TBASE__
    {
        {"gts_acquire_gap", PGC_SIGHUP, WAL_SETTINGS,
            gettext_noop("Time gap between global timestamp acquirement in the WAL writer."),
            NULL,
            GUC_UNIT_S
        },
        &WalGTSAcquireDelay,
        30, 10, 1800,
        NULL, NULL, NULL
    },
#endif

    {
        {"wal_writer_flush_after", PGC_SIGHUP, WAL_SETTINGS,
            gettext_noop("Amount of WAL written out by WAL writer that triggers a flush."),
            NULL,
            GUC_UNIT_XBLOCKS
        },
        &WalWriterFlushAfter,
        (1024 * 1024) / XLOG_BLCKSZ, 0, INT_MAX,
        NULL, NULL, NULL
    },

    {
        /* see max_connections */
        {"max_wal_senders", PGC_POSTMASTER, REPLICATION_SENDING,
            gettext_noop("Sets the maximum number of simultaneously running WAL sender processes."),
            NULL
        },
        &max_wal_senders,
        64, 0, MAX_BACKENDS,
        NULL, NULL, NULL
    },

    {
        /* see max_connections */
        {"max_replication_slots", PGC_POSTMASTER, REPLICATION_SENDING,
            gettext_noop("Sets the maximum number of simultaneously defined replication slots."),
            NULL
        },
        &max_replication_slots,
        64, 0, MAX_BACKENDS /* XXX? */ ,
        NULL, NULL, NULL
    },

    {
        {"wal_sender_timeout", PGC_SIGHUP, REPLICATION_SENDING,
            gettext_noop("Sets the maximum time to wait for WAL replication."),
            NULL,
            GUC_UNIT_MS
        },
        &wal_sender_timeout,
        60 * 1000, 0, INT_MAX,
        NULL, NULL, NULL
    },

    {
        {"commit_delay", PGC_SUSET, WAL_SETTINGS,
            gettext_noop("Sets the delay in microseconds between transaction commit and "
                         "flushing WAL to disk."),
            NULL
            /* we have no microseconds designation, so can't supply units here */
        },
        &CommitDelay,
        0, 0, 100000,
        NULL, NULL, NULL
    },

    {
        {"commit_siblings", PGC_USERSET, WAL_SETTINGS,
            gettext_noop("Sets the minimum concurrent open transactions before performing "
                         "commit_delay."),
            NULL
        },
        &CommitSiblings,
        5, 0, 1000,
        NULL, NULL, NULL
    },

    {
        {"extra_float_digits", PGC_USERSET, CLIENT_CONN_LOCALE,
            gettext_noop("Sets the number of digits displayed for floating-point values."),
            gettext_noop("This affects real, double precision, and geometric data types. "
                         "The parameter value is added to the standard number of digits "
                         "(FLT_DIG or DBL_DIG as appropriate).")
        },
        &extra_float_digits,
        0, -15, 3,
        NULL, NULL, NULL
    },

    {
        {"log_min_duration_statement", PGC_SUSET, LOGGING_WHEN,
            gettext_noop("Sets the minimum execution time above which "
                         "statements will be logged."),
            gettext_noop("Zero prints all queries. -1 turns this feature off."),
            GUC_UNIT_MS
        },
        &log_min_duration_statement,
        -1, -1, INT_MAX,
        NULL, NULL, NULL
    },

    {
        {"log_autovacuum_min_duration", PGC_SIGHUP, LOGGING_WHAT,
            gettext_noop("Sets the minimum execution time above which "
                         "autovacuum actions will be logged."),
            gettext_noop("Zero prints all actions. -1 turns autovacuum logging off."),
            GUC_UNIT_MS
        },
        &Log_autovacuum_min_duration,
        -1, -1, INT_MAX,
        NULL, NULL, NULL
    },

    {
        {"bgwriter_delay", PGC_SIGHUP, RESOURCES_BGWRITER,
            gettext_noop("Background writer sleep time between rounds."),
            NULL,
            GUC_UNIT_MS
        },
        &BgWriterDelay,
        200, 10, 10000,
        NULL, NULL, NULL
    },

    {
        {"bgwriter_lru_maxpages", PGC_SIGHUP, RESOURCES_BGWRITER,
            gettext_noop("Background writer maximum number of LRU pages to flush per round."),
            NULL
        },
        &bgwriter_lru_maxpages,
        100, 0, INT_MAX / 2,    /* Same upper limit as shared_buffers */
        NULL, NULL, NULL
    },

    {
        {"bgwriter_flush_after", PGC_SIGHUP, RESOURCES_BGWRITER,
            gettext_noop("Number of pages after which previously performed writes are flushed to disk."),
            NULL,
            GUC_UNIT_BLOCKS
        },
        &bgwriter_flush_after,
        DEFAULT_BGWRITER_FLUSH_AFTER, 0, WRITEBACK_MAX_PENDING_FLUSHES,
        NULL, NULL, NULL
    },

    {
        {"effective_io_concurrency",
            PGC_USERSET,
            RESOURCES_ASYNCHRONOUS,
            gettext_noop("Number of simultaneous requests that can be handled efficiently by the disk subsystem."),
            gettext_noop("For RAID arrays, this should be approximately the number of drive spindles in the array.")
        },
        &effective_io_concurrency,
#ifdef USE_PREFETCH
        1, 0, MAX_IO_CONCURRENCY,
#else
        0, 0, 0,
#endif
        check_effective_io_concurrency, assign_effective_io_concurrency, NULL
    },

    {
        {"backend_flush_after", PGC_USERSET, RESOURCES_ASYNCHRONOUS,
            gettext_noop("Number of pages after which previously performed writes are flushed to disk."),
            NULL,
            GUC_UNIT_BLOCKS
        },
        &backend_flush_after,
        DEFAULT_BACKEND_FLUSH_AFTER, 0, WRITEBACK_MAX_PENDING_FLUSHES,
        NULL, NULL, NULL
    },

    {
        {"max_worker_processes",
            PGC_POSTMASTER,
            RESOURCES_ASYNCHRONOUS,
            gettext_noop("Maximum number of concurrent worker processes."),
            NULL,
        },
        &max_worker_processes,
        128, 0, MAX_BACKENDS,
        check_max_worker_processes, NULL, NULL
    },

    {
        {"max_logical_replication_workers",
            PGC_POSTMASTER,
            REPLICATION_SUBSCRIBERS,
            gettext_noop("Maximum number of logical replication worker processes."),
            NULL,
        },
        &max_logical_replication_workers,
        64, 0, MAX_BACKENDS,
        NULL, NULL, NULL
    },

    {
        {"max_sync_workers_per_subscription",
            PGC_SIGHUP,
            REPLICATION_SUBSCRIBERS,
            gettext_noop("Maximum number of table synchronization workers per subscription."),
            NULL,
        },
        &max_sync_workers_per_subscription,
        4, 0, MAX_BACKENDS,
        NULL, NULL, NULL
    },
#ifdef __TBASE__
    {
        {"max_network_bandwidth_per_subscription",
            PGC_SIGHUP,
            REPLICATION_SUBSCRIBERS,
            gettext_noop("Maximum network bandwidth per subscription."),
            NULL,
        },
        &max_network_bandwidth_per_subscription,
        50, 10, INT_MAX,
        NULL, NULL, NULL
    },
        
    {
            {"base_backup_limit", PGC_SIGHUP, RESOURCES_KERNEL,
                    gettext_noop("basebackup speed limit."),
            },
            &g_TransferSpeed,
            50, 1, 10240,
            NULL, NULL, NULL
    },

    {
        {"max_sessions_per_shardpool", PGC_POSTMASTER, STATS_COLLECTOR,
            gettext_noop("max number of sessions for each shard pool."),
            gettext_noop("shard pool is used to store the statistic info about each shard from" 
                         " 0 to maxshards."),
        },
        &g_MaxSessionsPerPool,
        100, 10, 250,
        NULL, NULL, NULL
    },
    {
        {"shard_statistic_flush_interval", PGC_SIGHUP, STATS_COLLECTOR,
            gettext_noop("interval of flushing shard statistic."),
            NULL
        },
        &g_ShardInfoFlushInterval,
		300, 3, INT_MAX,
        NULL, NULL, NULL
    },

    {
        {"interval_sample_threshold", PGC_USERSET, RESOURCES,
            gettext_noop("sample threshold of interval partition."),
            NULL
        },
        &ParentTablePageSampleThreshold,
        100, 0, INT_MAX,
        NULL, NULL, NULL
    },

    {
        {"interval_sample_rate", PGC_USERSET, RESOURCES,
            gettext_noop("sample rate of interval partition."),
            NULL
        },
        &ParentTablePageSampleRate,
        20, 1, 100,
        NULL, NULL, NULL
    },

    {
        {"standby_plane_query_delay", PGC_USERSET, RESOURCES,
            gettext_noop("delay time of query in standby."),
            NULL
        },
        &query_delay,
        0, 0, 31536000,
        NULL, NULL, NULL
    },
	{
		{"max_relcache_relations", PGC_POSTMASTER, RESOURCES,
			gettext_noop("max relcache relations per session."),
			NULL
		},
		&max_relcache_relations,
#ifdef _PG_REGRESS_
		500, 500, INT_MAX,
#else
		2000, 500, INT_MAX,
#endif
		NULL, NULL, NULL
	},
	{
		{"number_replaced_relations", PGC_POSTMASTER, RESOURCES,
			gettext_noop("max relcache relations while replacing."),
			NULL
		},
		&number_replaced_relations,
		10, 1, 500,
		NULL, NULL, NULL
	},
#endif
    {
        {"log_rotation_age", PGC_SIGHUP, LOGGING_WHERE,
            gettext_noop("Automatic log file rotation will occur after N minutes."),
            NULL,
            GUC_UNIT_MIN
        },
        &Log_RotationAge,
        HOURS_PER_DAY * MINS_PER_HOUR, 0, INT_MAX / SECS_PER_MINUTE,
        NULL, NULL, NULL
    },

    {
        {"log_rotation_size", PGC_SIGHUP, LOGGING_WHERE,
            gettext_noop("Automatic log file rotation will occur after N kilobytes."),
            NULL,
            GUC_UNIT_KB
        },
        &Log_RotationSize,
        10 * 1024, 0, INT_MAX / 1024,
        NULL, NULL, NULL
    },
#ifdef __AUDIT__
    {
        {"alog_rotation_age", PGC_SIGHUP, LOGGING_WHERE,
            gettext_noop("Automatic audit log file rotation will occur after N minutes."),
            NULL,
            GUC_UNIT_MIN
        },
        &AuditLog_RotationAge,
        HOURS_PER_DAY * MINS_PER_HOUR, 0, INT_MAX / SECS_PER_MINUTE,
        NULL, NULL, NULL
    },
    {
        {"alog_rotation_size", PGC_SIGHUP, LOGGING_WHERE,
            gettext_noop("Automatic audit log file rotation will occur after N kilobytes."),
            NULL,
            GUC_UNIT_KB
        },
        &AuditLog_RotationSize,
        10 * 1024, 0, INT_MAX / 1024,
        NULL, NULL, NULL
    },
    {
        {"alog_max_worker_number", PGC_POSTMASTER, LOGGING_WHERE,
            gettext_noop("Max number of worker thead to read audit log from shared memory queue."),
            NULL,
        },
        &AuditLog_max_worker_number,
        16, 8, MAX_BACKENDS,
        NULL, NULL, NULL
    },
    {
        {"alog_common_queue_size", PGC_POSTMASTER, LOGGING_WHERE,
            gettext_noop("Size of share memory queue for each backend to store common audit log, kilobytes."),
            NULL,
            GUC_UNIT_KB
        },
        &AuditLog_common_log_queue_size_kb,
        64, 8, INT_MAX / 1024,
        NULL, NULL, NULL
    },
    {
        {"alog_fga_queue_size", PGC_POSTMASTER, LOGGING_WHERE,
            gettext_noop("Size of share memory queue for each backend to store fga audit log, kilobytes."),
            NULL,
            GUC_UNIT_KB
        },
        &AuditLog_fga_log_queue_size_kb,
        64, 8, INT_MAX / 1024,
        NULL, NULL, NULL
    },
    {
		{"alog_trace_queue_size", PGC_POSTMASTER, LOGGING_WHERE,
			gettext_noop("Size of share memory queue for each backend to store trace audit log, kilobytes."),
			NULL,
			GUC_UNIT_KB
		},
		&Maintain_trace_log_queue_size_kb,
		64, 8, INT_MAX / 1024,
		NULL, NULL, NULL
	},
    {
        {"alog_common_cache_size", PGC_POSTMASTER, LOGGING_WHERE,
            gettext_noop("Size of common audit log local buffer for each audit worker, kilobytes."),
            NULL,
            GUC_UNIT_KB
        },
        &AuditLog_common_log_cache_size_kb,
        64, 8, INT_MAX / 1024,
        NULL, NULL, NULL
    },
    {
        {"alog_fga_cacae_size", PGC_POSTMASTER, LOGGING_WHERE,
            gettext_noop("Size of fga audit log local buffer for each audit worker, kilobytes."),
            NULL,
            GUC_UNIT_KB
        },
        &AuditLog_fga_log_cacae_size_kb,
        64, 8, INT_MAX / 1024,
        NULL, NULL, NULL
    },
    {
		{"alog_trace_cache_size", PGC_POSTMASTER, LOGGING_WHERE,
			gettext_noop("Size of trace audit log local buffer for each audit worker, kilobytes."),
			NULL,
			GUC_UNIT_KB
		},
		&Maintain_trace_log_cache_size_kb,
		64, 8, INT_MAX / 1024,
		NULL, NULL, NULL
	},
#endif
    {
        {"max_function_args", PGC_INTERNAL, PRESET_OPTIONS,
            gettext_noop("Shows the maximum number of function arguments."),
            NULL,
            GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
        },
        &max_function_args,
        FUNC_MAX_ARGS, FUNC_MAX_ARGS, FUNC_MAX_ARGS,
        NULL, NULL, NULL
    },

    {
        {"max_index_keys", PGC_INTERNAL, PRESET_OPTIONS,
            gettext_noop("Shows the maximum number of index keys."),
            NULL,
            GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
        },
        &max_index_keys,
        INDEX_MAX_KEYS, INDEX_MAX_KEYS, INDEX_MAX_KEYS,
        NULL, NULL, NULL
    },

    {
        {"max_identifier_length", PGC_INTERNAL, PRESET_OPTIONS,
            gettext_noop("Shows the maximum identifier length."),
            NULL,
            GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
        },
        &max_identifier_length,
        NAMEDATALEN - 1, NAMEDATALEN - 1, NAMEDATALEN - 1,
        NULL, NULL, NULL
    },

    {
        {"block_size", PGC_INTERNAL, PRESET_OPTIONS,
            gettext_noop("Shows the size of a disk block."),
            NULL,
            GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
        },
        &block_size,
        BLCKSZ, BLCKSZ, BLCKSZ,
        NULL, NULL, NULL
    },

    {
        {"segment_size", PGC_INTERNAL, PRESET_OPTIONS,
            gettext_noop("Shows the number of pages per disk file."),
            NULL,
            GUC_UNIT_BLOCKS | GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
        },
        &segment_size,
        RELSEG_SIZE, RELSEG_SIZE, RELSEG_SIZE,
        NULL, NULL, NULL
    },

    {
        {"wal_block_size", PGC_INTERNAL, PRESET_OPTIONS,
            gettext_noop("Shows the block size in the write ahead log."),
            NULL,
            GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
        },
        &wal_block_size,
        XLOG_BLCKSZ, XLOG_BLCKSZ, XLOG_BLCKSZ,
        NULL, NULL, NULL
    },

    {
        {"wal_retrieve_retry_interval", PGC_SIGHUP, REPLICATION_STANDBY,
            gettext_noop("Sets the time to wait before retrying to retrieve WAL "
                         "after a failed attempt."),
            NULL,
            GUC_UNIT_MS
        },
        &wal_retrieve_retry_interval,
        5000, 1, INT_MAX,
        NULL, NULL, NULL
    },

    {
        {"wal_segment_size", PGC_INTERNAL, PRESET_OPTIONS,
            gettext_noop("Shows the number of pages per write ahead log segment."),
            NULL,
            GUC_UNIT_XBLOCKS | GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
        },
        &wal_segment_size,
        (XLOG_SEG_SIZE / XLOG_BLCKSZ),
        (XLOG_SEG_SIZE / XLOG_BLCKSZ),
        (XLOG_SEG_SIZE / XLOG_BLCKSZ),
        NULL, NULL, NULL
    },
#ifdef __TBASE__
    {
        {"wal_track_entry_number", PGC_POSTMASTER, REPLICATION_STANDBY,
            gettext_noop("Number of entries to track GTS of each xlog segment."),
            NULL,
            GUC_UNIT_MS
        },
        &wal_gts_track_entries,
        256, 16, INT_MAX,
        NULL, NULL, NULL
    },    
#endif

    {
        {"autovacuum_naptime", PGC_SIGHUP, AUTOVACUUM,
            gettext_noop("Time to sleep between autovacuum runs."),
            NULL,
            GUC_UNIT_S
        },
        &autovacuum_naptime,
        60, 1, INT_MAX / 1000,
        NULL, NULL, NULL
    },
    {
        {"autovacuum_vacuum_threshold", PGC_SIGHUP, AUTOVACUUM,
            gettext_noop("Minimum number of tuple updates or deletes prior to vacuum."),
            NULL
        },
        &autovacuum_vac_thresh,
        50, 0, INT_MAX,
        NULL, NULL, NULL
    },
    {
        {"autovacuum_analyze_threshold", PGC_SIGHUP, AUTOVACUUM,
            gettext_noop("Minimum number of tuple inserts, updates, or deletes prior to analyze."),
            NULL
        },
        &autovacuum_anl_thresh,
        50, 0, INT_MAX,
        NULL, NULL, NULL
    },
    {
        /* see varsup.c for why this is PGC_POSTMASTER not PGC_SIGHUP */
        {"autovacuum_freeze_max_age", PGC_POSTMASTER, AUTOVACUUM,
            gettext_noop("Age at which to autovacuum a table to prevent transaction ID wraparound."),
            NULL
        },
        &autovacuum_freeze_max_age,
        /* see pg_resetwal if you change the upper-limit value */
        200000000, 100000, 2000000000,
        NULL, NULL, NULL
    },
    {
        /* see multixact.c for why this is PGC_POSTMASTER not PGC_SIGHUP */
        {"autovacuum_multixact_freeze_max_age", PGC_POSTMASTER, AUTOVACUUM,
            gettext_noop("Multixact age at which to autovacuum a table to prevent multixact wraparound."),
            NULL
        },
        &autovacuum_multixact_freeze_max_age,
        400000000, 10000, 2000000000,
        NULL, NULL, NULL
    },
    {
        /* see max_connections */
        {"autovacuum_max_workers", PGC_POSTMASTER, AUTOVACUUM,
            gettext_noop("Sets the maximum number of simultaneously running autovacuum worker processes."),
            NULL
        },
        &autovacuum_max_workers,
        3, 1, MAX_BACKENDS,
        check_autovacuum_max_workers, NULL, NULL
    },

    {
        {"max_parallel_workers_per_gather", PGC_USERSET, RESOURCES_ASYNCHRONOUS,
            gettext_noop("Sets the maximum number of parallel processes per executor node."),
            NULL
        },
        &max_parallel_workers_per_gather,
        2, 0, MAX_PARALLEL_WORKER_LIMIT,
        NULL, NULL, NULL
    },

    {
        {"max_parallel_workers", PGC_USERSET, RESOURCES_ASYNCHRONOUS,
            gettext_noop("Sets the maximum number of parallel workers than can be active at one time."),
            NULL
        },
        &max_parallel_workers,
        64, 0, MAX_PARALLEL_WORKER_LIMIT,
        NULL, NULL, NULL
    },

    {
        {"autovacuum_work_mem", PGC_SIGHUP, RESOURCES_MEM,
            gettext_noop("Sets the maximum memory to be used by each autovacuum worker process."),
            NULL,
            GUC_UNIT_KB
        },
        &autovacuum_work_mem,
        -1, -1, MAX_KILOBYTES,
        check_autovacuum_work_mem, NULL, NULL
    },

    {
        {"old_snapshot_threshold", PGC_POSTMASTER, RESOURCES_ASYNCHRONOUS,
            gettext_noop("Time before a snapshot is too old to read pages changed after the snapshot was taken."),
            gettext_noop("A value of -1 disables this feature."),
            GUC_UNIT_MIN
        },
        &old_snapshot_threshold,
        -1, -1, MINS_PER_HOUR * HOURS_PER_DAY * 60,
        NULL, NULL, NULL
    },

    {
        {"tcp_keepalives_idle", PGC_USERSET, CLIENT_CONN_OTHER,
            gettext_noop("Time between issuing TCP keepalives."),
            gettext_noop("A value of 0 uses the system default."),
            GUC_UNIT_S
        },
        &tcp_keepalives_idle,
        0, 0, INT_MAX,
        NULL, assign_tcp_keepalives_idle, show_tcp_keepalives_idle
    },

    {
        {"tcp_keepalives_interval", PGC_USERSET, CLIENT_CONN_OTHER,
            gettext_noop("Time between TCP keepalive retransmits."),
            gettext_noop("A value of 0 uses the system default."),
            GUC_UNIT_S
        },
        &tcp_keepalives_interval,
        0, 0, INT_MAX,
        NULL, assign_tcp_keepalives_interval, show_tcp_keepalives_interval
    },

    {
        {"ssl_renegotiation_limit", PGC_USERSET, CONN_AUTH_SECURITY,
            gettext_noop("SSL renegotiation is no longer supported; this can only be 0."),
            NULL,
            GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE,
        },
        &ssl_renegotiation_limit,
        0, 0, 0,
        NULL, NULL, NULL
    },

    {
        {"tcp_keepalives_count", PGC_USERSET, CLIENT_CONN_OTHER,
            gettext_noop("Maximum number of TCP keepalive retransmits."),
            gettext_noop("This controls the number of consecutive keepalive retransmits that can be "
                         "lost before a connection is considered dead. A value of 0 uses the "
                         "system default."),
        },
        &tcp_keepalives_count,
        0, 0, INT_MAX,
        NULL, assign_tcp_keepalives_count, show_tcp_keepalives_count
    },

    {
        {"gin_fuzzy_search_limit", PGC_USERSET, CLIENT_CONN_OTHER,
            gettext_noop("Sets the maximum allowed result for exact search by GIN."),
            NULL,
            0
        },
        &GinFuzzySearchLimit,
        0, 0, INT_MAX,
        NULL, NULL, NULL
    },

    {
        {"effective_cache_size", PGC_USERSET, QUERY_TUNING_COST,
            gettext_noop("Sets the planner's assumption about the size of the disk cache."),
            gettext_noop("That is, the portion of the kernel's disk cache that "
                         "will be used for PostgreSQL data files. This is measured in disk "
                         "pages, which are normally 8 kB each."),
            GUC_UNIT_BLOCKS,
        },
        &effective_cache_size,
        DEFAULT_EFFECTIVE_CACHE_SIZE, 1, INT_MAX,
        NULL, NULL, NULL
    },

    {
        {"min_parallel_table_scan_size", PGC_USERSET, QUERY_TUNING_COST,
            gettext_noop("Sets the minimum amount of table data for a parallel scan."),
            gettext_noop("If the planner estimates that it will read a number of table pages too small to reach this limit, a parallel scan will not be considered."),
            GUC_UNIT_BLOCKS,
        },
        &min_parallel_table_scan_size,
        (8 * 1024 * 1024) / BLCKSZ, 0, INT_MAX / 3,
        NULL, NULL, NULL
    },

    {
        {"min_parallel_index_scan_size", PGC_USERSET, QUERY_TUNING_COST,
            gettext_noop("Sets the minimum amount of index data for a parallel scan."),
            gettext_noop("If the planner estimates that it will read a number of index pages too small to reach this limit, a parallel scan will not be considered."),
            GUC_UNIT_BLOCKS,
        },
        &min_parallel_index_scan_size,
        (512 * 1024) / BLCKSZ, 0, INT_MAX / 3,
        NULL, NULL, NULL
    },
#ifdef __TBASE__
	{
		{"min_parallel_rows_size", PGC_USERSET, QUERY_TUNING_COST,
			gettext_noop("Sets the minimum amount of rows for a parallel aggregate or scan."),
			gettext_noop("If the planner estimates that it will read rows too small to reach this limit, a parallel plan will not be considered.")
		},
		&min_parallel_rows_size,
		50000, 0, INT_MAX / 3,
		NULL, NULL, NULL
	},
#endif
    {
        /* Can't be set in postgresql.conf */
        {"server_version_num", PGC_INTERNAL, PRESET_OPTIONS,
            gettext_noop("Shows the server version as an integer."),
            NULL,
            GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
        },
        &server_version_num,
        PG_VERSION_NUM, PG_VERSION_NUM, PG_VERSION_NUM,
        NULL, NULL, NULL
    },

    {
        {"log_temp_files", PGC_SUSET, LOGGING_WHAT,
            gettext_noop("Log the use of temporary files larger than this number of kilobytes."),
            gettext_noop("Zero logs all files. The default is -1 (turning this feature off)."),
            GUC_UNIT_KB
        },
        &log_temp_files,
        -1, -1, INT_MAX,
        NULL, NULL, NULL
    },

    {
        {"track_activity_query_size", PGC_POSTMASTER, RESOURCES_MEM,
            gettext_noop("Sets the size reserved for pg_stat_activity.query, in bytes."),
            NULL,

            /*
             * There is no _bytes_ unit, so the user can't supply units for
             * this.
             */
        },
        &pgstat_track_activity_query_size,
        1024, 100, 102400,
        NULL, NULL, NULL
    },
#ifdef PGXC
    {
        {"sequence_range", PGC_USERSET, COORDINATORS,
            gettext_noop("The range of values to ask from GTM for sequences. "
                         "If CACHE parameter is set then that overrides this."),
            NULL,
        },
        &SequenceRangeVal,
        1000, 1, INT_MAX,
        NULL, NULL, NULL
    },

#ifdef __TBASE__
    {
        {"pool_conn_keepalive", PGC_SIGHUP, DATA_NODES,
            gettext_noop("Close connections if they are idle in the pool for that time."),
            gettext_noop("A value of -1 turns autoclose off."),
            GUC_UNIT_S
        },
        &PoolConnKeepAlive,
        60, 60, INT_MAX,
        NULL, NULL, NULL
    },

    {
        {"pool_maintenance_timeout", PGC_SIGHUP, DATA_NODES,
            gettext_noop("Launch maintenance routine if pooler idle for that time."),
            gettext_noop("A value of -1 turns feature off."),
            GUC_UNIT_S
        },
        &PoolMaintenanceTimeout,
        10, -1, INT_MAX,
        NULL, NULL, NULL
    },

    {
        {"max_pool_size", PGC_POSTMASTER, DATA_NODES,
            gettext_noop("Max pool size."),
            gettext_noop("If number of active connections reaches this value, "
                         "other connection requests will be refused")
        },
        &MaxPoolSize,
        300, 1, 65535,
        NULL, NULL, NULL
    },
    {
        {"init_pool_size", PGC_POSTMASTER, DATA_NODES,
            gettext_noop("Initial pool size."),
            gettext_noop("When a new user comes in, we precreate the number of connections for him ")
        },
        &InitPoolSize,
        10, 1, 65535,
        NULL, NULL, NULL
    },        
    {
        {"min_free_size", PGC_POSTMASTER, DATA_NODES,
            gettext_noop("minimal pool free connection number."),
            gettext_noop("When pool need to acquire new connections, we use the number as step ")
        },
        &MinFreeSize,
        5, 1, 65535,
        NULL, NULL, NULL
    },    
    {
        {"min_pool_size", PGC_POSTMASTER, DATA_NODES,
            gettext_noop("Min pool size."),
            gettext_noop("If number of active connections decreased below this value, "
                         "we established new connections for warm user and database")
        },
        &MinPoolSize,
        5, 1, 65535,
        NULL, NULL, NULL
    },
    {
        {"pool_session_context_check_gap", PGC_SIGHUP, DATA_NODES,
            gettext_noop("Gap to check datanode session memory context."),
            gettext_noop("In seconds."),
            GUC_UNIT_S
        },
        &PoolSizeCheckGap,
        120, 10, 7200,
        NULL, NULL, NULL
    },
    {
        {"pool_session_max_lifetime", PGC_SIGHUP, DATA_NODES,
            gettext_noop("Datanode session max lifetime."),
            gettext_noop("Session will be colsed when expired."),
            GUC_UNIT_S
        },
        &PoolConnMaxLifetime,
        300, 1, INT_MAX,
        NULL, NULL, NULL
    },
    {
        {"pool_session_memory_limit", PGC_SIGHUP, DATA_NODES,
            gettext_noop("Datanode session max memory context size."),
            gettext_noop("Exceed limit will be closed."),
			GUC_UNIT_MB
        },
        &PoolMaxMemoryLimit,
		10, -1, 10000,
        NULL, NULL, NULL
    },    
    {
        {"pooler_connect_timeout", PGC_POSTMASTER, DATA_NODES,
            gettext_noop("Pooler connection timeout."),
            gettext_noop("Pooler connection timeout."),
            GUC_UNIT_S
        },
        &PoolConnectTimeOut,
        10, 1, 3600,
        NULL, NULL, NULL
    },    
    {
        {"pooler_scale_factor", PGC_POSTMASTER, DATA_NODES,
            gettext_noop("Pooler scale factor."),
            gettext_noop("Pooler scale factor.")
        },
        &PoolScaleFactor,
        2, 1, 64,
        NULL, NULL, NULL
    },    
    {
        {"pooler_dn_set_timeout", PGC_SIGHUP, DATA_NODES,
            gettext_noop("Pooler datanode set query timeout."),
            gettext_noop("Pooler datanode set query timeout."),
            GUC_UNIT_S
        },
        &PoolDNSetTimeout,
        10, 1, 3600,
        NULL, NULL, NULL
    },    
    {
        {"pool_check_slot_timeout", PGC_SIGHUP, DATA_NODES,
            gettext_noop("Enable pooler check slot. When slot is using by agent, shouldn't exist in nodepool."),
            gettext_noop("A value of -1 turns feature off."),
            GUC_UNIT_S
        },
        &PoolCheckSlotTimeout,
        5, -1, INT_MAX,
        NULL, NULL, NULL
    },
    {
        {"pool_print_stat_timeout", PGC_SIGHUP, DATA_NODES,
            gettext_noop("Enable pooler print stat info."),
            gettext_noop("A value of -1 turns feature off."),
            GUC_UNIT_S
        },
        &PoolPrintStatTimeout,
        60, -1, INT_MAX,
        NULL, NULL, NULL
    },
    {
        {"session_memory_size", PGC_USERSET, RESOURCES_MEM,
            gettext_noop("Used to get the total memory size of the session, in M Bytes."),
			gettext_noop("Used to get the total memory size of the session, in M Bytes."),
			GUC_UNIT_MB
        },
        &g_TotalMemorySize,
        0, 0, INT_MAX,
        NULL, NULL, show_total_memorysize
    },
#endif
#ifdef _MLS_
    {
        {"checkpoint_crypt_worker", PGC_POSTMASTER, DATA_NODES,
            gettext_noop("number of workers for parellel crypt."),
            gettext_noop("min 1, max 24, default 4.")
        },
        &g_checkpoint_crypt_worker,
        4, 1, 24,
        NULL, NULL, NULL
    },
    {
        {"checkpoint_crypt_queue_length", PGC_POSTMASTER, DATA_NODES,
            gettext_noop("length of crypt queue between checkpoint main and crypt workers."),
            gettext_noop("min 4, max 64, default 32.")
        },
        &g_checkpoint_crypt_queue_length,
        32, 4, 64,
        NULL, NULL, NULL
    },
#endif
    {
        {"pooler_port", PGC_POSTMASTER, DATA_NODES,
            gettext_noop("Port of the Pool Manager."),
            NULL
        },
        &PoolerPort,
        6667, 1, 65535,
        NULL, NULL, NULL
    },
#ifdef XCP
    /*
     * Shared queues provide shared memory buffers to stream data from
     * "producer" - process which executes subplan to "consumers" - processes
     * that are forwarding data to destination data nodes.
     */
    {
        {"shared_queues", PGC_POSTMASTER, RESOURCES_MEM,
            gettext_noop("Sets the number of shared memory queues used by the "
                    "distributed executor, minimum 1/4 of max_connections."),
            NULL
        },
        &NSQueues,
        256, 16, INT_MAX,
        NULL, NULL, NULL
    },

    {
        {"shared_queue_size", PGC_POSTMASTER, RESOURCES_MEM,
            gettext_noop("Sets the amount of memory allocated for a shared"
                    " memory queue per datanode."),
            NULL,
            GUC_UNIT_KB
        },
        &SQueueSize,
        32, 1, MAX_KILOBYTES,
        NULL, NULL, NULL
    },

    {
        {"parentPGXCPid", PGC_USERSET, UNGROUPED,
            gettext_noop("PID of the remote process attached to this session."),
            gettext_noop("This GUC only makes sense when a coordinator or a "
                    "datanode has opened the session"),
            GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_AUTO_FILE | GUC_DISALLOW_IN_FILE | GUC_NO_SHOW_ALL
        },
        &parentPGXCPid,
        -1, -1, INT_MAX,
        NULL, NULL, NULL
    },

    {
        {"pgxl_remote_fetch_size", PGC_USERSET, UNGROUPED,
            gettext_noop("Number of maximum tuples to fetch in one remote iteration"),
            NULL,
            0
        },
        &PGXLRemoteFetchSize,
        1000, 1, INT_MAX,
        NULL, NULL, NULL
    },
#ifdef __TBASE__
    {
        {"sender_thread_num", PGC_SIGHUP, CUSTOM_OPTIONS,
            gettext_noop("Number of maximum senders in datapump"),
            NULL,
            0
        },
        &g_SndThreadNum,
        8, 1, 512,
        NULL, NULL, NULL
    },
    {
        {"consumer_connect_timeout", PGC_SIGHUP, CUSTOM_OPTIONS,
            gettext_noop("timeout to comsumer connect to producer"),
            NULL,
            0
        },
        &consumer_connect_timeout,
        60, 1, INT_MAX,
        NULL, NULL, NULL
    },
    {
        {"dis_consumer_timeout", PGC_SIGHUP, CUSTOM_OPTIONS,
            gettext_noop("timeout to vacuum disconnected comsumer's entry"),
            NULL,
            0
        },
        &g_DisConsumer_timeout,
        60, 0, INT_MAX,
        NULL, NULL, NULL
    },
    {
        {"sender_thread_buffer_size", PGC_SIGHUP, CUSTOM_OPTIONS,
            gettext_noop("buffer size of senders in datapump"),
            NULL,
            0
        },
        &g_SndThreadBufferSize,
        16, 1, 1048576,
        NULL, NULL, NULL
    },
    {
        {"sender_thread_batch_size", PGC_SIGHUP, CUSTOM_OPTIONS,
            gettext_noop("batch size of senders in datapump"),
            NULL,
            0
        },
        &g_SndBatchSize,
        8, 1, 524288,
        NULL, NULL, NULL
    },
    {
        {"archive_autowake_interval", PGC_USERSET, WAL_ARCHIVING,
            gettext_noop("how often to force a poll of the archive status directory in seconds."),
        },
        &archive_autowake_interval,
        30, 1, 60,
        NULL, NULL, NULL
    },
    {
        {"datarow_buffer_size", PGC_SIGHUP, CUSTOM_OPTIONS,
            gettext_noop("buffer size for prefetch."),
        },
        &DataRowBufferSize,
        32, 0, INT_MAX,
        NULL, NULL, NULL
    },

    {
        {"replication_level", PGC_USERSET, CUSTOM_OPTIONS,
            gettext_noop("replication level on join to make Query more efficient."),
            NULL
        },
        &replication_level,
        1, 0, INT_MAX,
        NULL, NULL, NULL
    },
	
	{
		{"default_hashagg_nbatches", PGC_USERSET, CUSTOM_OPTIONS,
			gettext_noop("number of batch files in hybrid-hash agg."),
			NULL
		},
		&g_default_hashagg_nbatches,
		32, 1, INT_MAX,
		NULL, NULL, NULL
	},
#endif
#ifdef __TWO_PHASE_TESTS__
    {
        {"twophase_exception_case", PGC_USERSET, CUSTOM_OPTIONS,
            gettext_noop("run tests for twophase transaction"),
            NULL
        },
        &twophase_exception_case,
        0, 0, INT_MAX,
        NULL, NULL, NULL
    },
    {
        {"run_pg_clean", PGC_USERSET, CUSTOM_OPTIONS,
            gettext_noop("run tests on pg_clean"),
            NULL
        },
        &run_pg_clean,
        0, 0, 1,
        NULL, NULL, NULL
    },
#endif
#ifdef __STORAGE_SCALABLE__
    {
        {"pub_stat_hash_size", PGC_SIGHUP, RESOURCES,
            gettext_noop("size of hashtable used in publication statistics."),
            NULL
        },
        &g_PubStatHashSize,
        65536, 8192, INT_MAX,
        NULL, NULL, NULL
    },
    {
        {"pub_table_stat_hash_size", PGC_SIGHUP, RESOURCES,
            gettext_noop("size of hashtable used in publication's table statistics."),
            NULL
        },
        &g_PubTableStatHashSize,
        262144, 8192, INT_MAX,
        NULL, NULL, NULL
    },
    {
        {"sub_stat_hash_size", PGC_SIGHUP, RESOURCES,
            gettext_noop("size of hashtable used in subscription statistics."),
            NULL
        },
        &g_SubStatHashSize,
        65536, 8192, INT_MAX,
        NULL, NULL, NULL
    },
    {
        {"sub_table_stat_hash_size", PGC_SIGHUP, RESOURCES,
            gettext_noop("size of hashtable used in subscription's table statistics."),
            NULL
        },
        &g_SubTableStatHashSize,
        262144, 8192, INT_MAX,
        NULL, NULL, NULL
    },
#endif
#ifdef __COLD_HOT__
    {
        {"create_key_value_mode", PGC_USERSET, QUERY_TUNING,
            gettext_noop("To control key value created on nodes range, 0:all, 1:cn only, 2:dn only"),
        },
        &g_create_key_value_mode,
        0, 0, 2,
        NULL, NULL, NULL
    },
    {
        {"hot_data_age", PGC_SIGHUP, RESOURCES_KERNEL,
            gettext_noop("age of hot data, data older than this age will be turned cold."),
        },
        &g_ColdDataThreashold,
        3650000, 30, INT_MAX,
        NULL, NULL, NULL
    },    
#endif
#endif
#endif /* PGXC */
    {
        {"gin_pending_list_limit", PGC_USERSET, CLIENT_CONN_STATEMENT,
            gettext_noop("Sets the maximum size of the pending list for GIN index."),
            NULL,
            GUC_UNIT_KB
        },
        &gin_pending_list_limit,
        4096, 64, MAX_KILOBYTES,
        NULL, NULL, NULL
    },

#ifdef __TBASE__
    {
        {"account_lock_track_count", PGC_POSTMASTER, LOGGING,
            gettext_noop("The limit to store user account info. aka the max num of hash table entry."),
        },
        &pgua_max,
        1000, 1, INT_MAX,
        NULL, NULL, NULL
    },

    {
        {"account_lock_time", PGC_SUSET, LOGGING,
            gettext_noop("The time which user account will be locked."),
            gettext_noop("The time which user account will be locked."),
            GUC_UNIT_S
        },
        &account_lock_time,
        600, 1, INT_MAX,
        NULL, NULL, NULL
    },

    {
        {"account_lock_threshold", PGC_SUSET, LOGGING,
            gettext_noop("Account will be locked after failed  account_lock_threshold times"),
        },
        &account_lock_threshold,
        10, 1, INT_MAX,
        NULL, NULL, NULL
    },
#endif

    /* End-of-list marker */
    {
        {NULL, 0, 0, NULL, NULL}, NULL, 0, 0, 0, NULL, NULL, NULL
    }
};

static struct config_uint ConfigureNamesUInt[] =
{
    {
        {"coordinator_lxid", PGC_USERSET, UNGROUPED,
            gettext_noop("Sets the coordinator local transaction identifier."),
            NULL,
            GUC_IS_NAME | GUC_REPORT | GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE | GUC_NOT_WHILE_SEC_REST
        },
        &MyCoordLxid,
		0, 0, UINT_MAX,
        NULL, NULL, NULL
    },

    /* End-of-list marker */
    {
        {NULL, 0, 0, NULL, NULL}, NULL, 0, 0, 0, NULL, NULL, NULL
    }
};

static struct config_real ConfigureNamesReal[] =
{
    {
        {"seq_page_cost", PGC_USERSET, QUERY_TUNING_COST,
            gettext_noop("Sets the planner's estimate of the cost of a "
                         "sequentially fetched disk page."),
            NULL
        },
        &seq_page_cost,
        DEFAULT_SEQ_PAGE_COST, 0, DBL_MAX,
        NULL, NULL, NULL
    },
    {
        {"random_page_cost", PGC_USERSET, QUERY_TUNING_COST,
            gettext_noop("Sets the planner's estimate of the cost of a "
                         "nonsequentially fetched disk page."),
            NULL
        },
        &random_page_cost,
        DEFAULT_RANDOM_PAGE_COST, 0, DBL_MAX,
        NULL, NULL, NULL
    },
    {
        {"cpu_tuple_cost", PGC_USERSET, QUERY_TUNING_COST,
            gettext_noop("Sets the planner's estimate of the cost of "
                         "processing each tuple (row)."),
            NULL
        },
        &cpu_tuple_cost,
        DEFAULT_CPU_TUPLE_COST, 0, DBL_MAX,
        NULL, NULL, NULL
    },
    {
        {"cpu_index_tuple_cost", PGC_USERSET, QUERY_TUNING_COST,
            gettext_noop("Sets the planner's estimate of the cost of "
                         "processing each index entry during an index scan."),
            NULL
        },
        &cpu_index_tuple_cost,
        DEFAULT_CPU_INDEX_TUPLE_COST, 0, DBL_MAX,
        NULL, NULL, NULL
    },
    {
        {"cpu_operator_cost", PGC_USERSET, QUERY_TUNING_COST,
            gettext_noop("Sets the planner's estimate of the cost of "
                         "processing each operator or function call."),
            NULL
        },
        &cpu_operator_cost,
        DEFAULT_CPU_OPERATOR_COST, 0, DBL_MAX,
        NULL, NULL, NULL
    },
    {
        {"parallel_tuple_cost", PGC_USERSET, QUERY_TUNING_COST,
            gettext_noop("Sets the planner's estimate of the cost of "
                         "passing each tuple (row) from worker to master backend."),
            NULL
        },
        &parallel_tuple_cost,
        DEFAULT_PARALLEL_TUPLE_COST, 0, DBL_MAX,
        NULL, NULL, NULL
    },
    {
        {"parallel_setup_cost", PGC_USERSET, QUERY_TUNING_COST,
            gettext_noop("Sets the planner's estimate of the cost of "
                         "starting up worker processes for parallel query."),
            NULL
        },
        &parallel_setup_cost,
        DEFAULT_PARALLEL_SETUP_COST, 0, DBL_MAX,
        NULL, NULL, NULL
    },

    {
        {"cursor_tuple_fraction", PGC_USERSET, QUERY_TUNING_OTHER,
            gettext_noop("Sets the planner's estimate of the fraction of "
                         "a cursor's rows that will be retrieved."),
            NULL
        },
        &cursor_tuple_fraction,
        DEFAULT_CURSOR_TUPLE_FRACTION, 0.0, 1.0,
        NULL, NULL, NULL
    },

#ifdef XCP
    {
        {"network_byte_cost", PGC_USERSET, QUERY_TUNING_COST,
            gettext_noop("Sets the planner's estimate of the cost of "
                         "sending data from remote node."),
            NULL
        },
        &network_byte_cost,
        DEFAULT_NETWORK_BYTE_COST, 0, DBL_MAX, NULL, NULL
    },

    {
        {"remote_query_cost", PGC_USERSET, QUERY_TUNING_COST,
            gettext_noop("Sets the planner's estimate of the cost of "
                         "setting up remote subquery."),
            NULL
        },
        &remote_query_cost,
        DEFAULT_REMOTE_QUERY_COST, 0, DBL_MAX, NULL, NULL
    },
#endif

    {
        {"geqo_selection_bias", PGC_USERSET, QUERY_TUNING_GEQO,
            gettext_noop("GEQO: selective pressure within the population."),
            NULL
        },
        &Geqo_selection_bias,
        DEFAULT_GEQO_SELECTION_BIAS,
        MIN_GEQO_SELECTION_BIAS, MAX_GEQO_SELECTION_BIAS,
        NULL, NULL, NULL
    },
    {
        {"geqo_seed", PGC_USERSET, QUERY_TUNING_GEQO,
            gettext_noop("GEQO: seed for random path selection."),
            NULL
        },
        &Geqo_seed,
        0.0, 0.0, 1.0,
        NULL, NULL, NULL
    },

    {
        {"bgwriter_lru_multiplier", PGC_SIGHUP, RESOURCES_BGWRITER,
            gettext_noop("Multiple of the average buffer usage to free per round."),
            NULL
        },
        &bgwriter_lru_multiplier,
        2.0, 0.0, 10.0,
        NULL, NULL, NULL
    },

    {
        {"seed", PGC_USERSET, UNGROUPED,
            gettext_noop("Sets the seed for random-number generation."),
            NULL,
            GUC_NO_SHOW_ALL | GUC_NO_RESET_ALL | GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
        },
        &phony_random_seed,
        0.0, -1.0, 1.0,
        check_random_seed, assign_random_seed, show_random_seed
    },

    {
        {"autovacuum_vacuum_scale_factor", PGC_SIGHUP, AUTOVACUUM,
            gettext_noop("Number of tuple updates or deletes prior to vacuum as a fraction of reltuples."),
            NULL
        },
        &autovacuum_vac_scale,
        0.2, 0.0, 100.0,
        NULL, NULL, NULL
    },
    {
        {"autovacuum_analyze_scale_factor", PGC_SIGHUP, AUTOVACUUM,
            gettext_noop("Number of tuple inserts, updates, or deletes prior to analyze as a fraction of reltuples."),
            NULL
        },
        &autovacuum_anl_scale,
        0.1, 0.0, 100.0,
        NULL, NULL, NULL
    },

    {
        {"checkpoint_completion_target", PGC_SIGHUP, WAL_CHECKPOINTS,
            gettext_noop("Time spent flushing dirty buffers during checkpoint, as fraction of checkpoint interval."),
            NULL
        },
        &CheckPointCompletionTarget,
        0.5, 0.0, 1.0,
        NULL, NULL, NULL
    },

    /* End-of-list marker */
    {
        {NULL, 0, 0, NULL, NULL}, NULL, 0.0, 0.0, 0.0, NULL, NULL, NULL
    }
};

static struct config_string ConfigureNamesString[] =
{
    {
        {"archive_command", PGC_SIGHUP, WAL_ARCHIVING,
            gettext_noop("Sets the shell command that will be called to archive a WAL file."),
            NULL
        },
        &XLogArchiveCommand,
        "",
        NULL, NULL, show_archive_command
    },

    {
        {"client_encoding", PGC_USERSET, CLIENT_CONN_LOCALE,
            gettext_noop("Sets the client's character set encoding."),
            NULL,
            GUC_IS_NAME | GUC_REPORT
        },
        &client_encoding_string,
        "SQL_ASCII",
        check_client_encoding, assign_client_encoding, NULL
    },

    {
        {"log_line_prefix", PGC_SIGHUP, LOGGING_WHAT,
            gettext_noop("Controls information prefixed to each log line."),
            gettext_noop("If blank, no prefix is used.")
        },
        &Log_line_prefix,
        "%m [%p] ",
        NULL, NULL, NULL
    },

    {
        {"log_timezone", PGC_SIGHUP, LOGGING_WHAT,
            gettext_noop("Sets the time zone to use in log messages."),
            NULL
        },
        &log_timezone_string,
        "GMT",
        check_log_timezone, assign_log_timezone, show_log_timezone
    },

    {
        {"DateStyle", PGC_USERSET, CLIENT_CONN_LOCALE,
            gettext_noop("Sets the display format for date and time values."),
            gettext_noop("Also controls interpretation of ambiguous "
                         "date inputs."),
            GUC_LIST_INPUT | GUC_REPORT
        },
        &datestyle_string,
        "ISO, MDY",
        check_datestyle, assign_datestyle, NULL
    },

    {
        {"default_tablespace", PGC_USERSET, CLIENT_CONN_STATEMENT,
            gettext_noop("Sets the default tablespace to create tables and indexes in."),
            gettext_noop("An empty string selects the database's default tablespace."),
            GUC_IS_NAME
        },
        &default_tablespace,
        "",
        check_default_tablespace, NULL, NULL
    },

    {
        {"temp_tablespaces", PGC_USERSET, CLIENT_CONN_STATEMENT,
            gettext_noop("Sets the tablespace(s) to use for temporary tables and sort files."),
            NULL,
            GUC_LIST_INPUT | GUC_LIST_QUOTE
        },
        &temp_tablespaces,
        "",
        check_temp_tablespaces, assign_temp_tablespaces, NULL
    },

    {
        {"dynamic_library_path", PGC_SUSET, CLIENT_CONN_OTHER,
            gettext_noop("Sets the path for dynamically loadable modules."),
            gettext_noop("If a dynamically loadable module needs to be opened and "
                         "the specified name does not have a directory component (i.e., the "
                         "name does not contain a slash), the system will search this path for "
                         "the specified file."),
            GUC_SUPERUSER_ONLY
        },
        &Dynamic_library_path,
        "$libdir",
        NULL, NULL, NULL
    },

    {
        {"krb_server_keyfile", PGC_SIGHUP, CONN_AUTH_SECURITY,
            gettext_noop("Sets the location of the Kerberos server key file."),
            NULL,
            GUC_SUPERUSER_ONLY
        },
        &pg_krb_server_keyfile,
        PG_KRB_SRVTAB,
        NULL, NULL, NULL
    },

    {
        {"bonjour_name", PGC_POSTMASTER, CONN_AUTH_SETTINGS,
            gettext_noop("Sets the Bonjour service name."),
            NULL
        },
        &bonjour_name,
        "",
        NULL, NULL, NULL
    },

    /* See main.c about why defaults for LC_foo are not all alike */

    {
        {"lc_collate", PGC_INTERNAL, CLIENT_CONN_LOCALE,
            gettext_noop("Shows the collation order locale."),
            NULL,
            GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
        },
        &locale_collate,
        "C",
        NULL, NULL, NULL
    },

    {
        {"lc_ctype", PGC_INTERNAL, CLIENT_CONN_LOCALE,
            gettext_noop("Shows the character classification and case conversion locale."),
            NULL,
            GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
        },
        &locale_ctype,
        "C",
        NULL, NULL, NULL
    },

    {
        {"lc_messages", PGC_SUSET, CLIENT_CONN_LOCALE,
            gettext_noop("Sets the language in which messages are displayed."),
            NULL
        },
        &locale_messages,
        "",
        check_locale_messages, assign_locale_messages, NULL
    },

    {
        {"lc_monetary", PGC_USERSET, CLIENT_CONN_LOCALE,
            gettext_noop("Sets the locale for formatting monetary amounts."),
            NULL
        },
        &locale_monetary,
        "C",
        check_locale_monetary, assign_locale_monetary, NULL
    },

    {
        {"lc_numeric", PGC_USERSET, CLIENT_CONN_LOCALE,
            gettext_noop("Sets the locale for formatting numbers."),
            NULL
        },
        &locale_numeric,
        "C",
        check_locale_numeric, assign_locale_numeric, NULL
    },

    {
        {"lc_time", PGC_USERSET, CLIENT_CONN_LOCALE,
            gettext_noop("Sets the locale for formatting date and time values."),
            NULL
        },
        &locale_time,
        "C",
        check_locale_time, assign_locale_time, NULL
    },

    {
        {"session_preload_libraries", PGC_SUSET, CLIENT_CONN_PRELOAD,
            gettext_noop("Lists shared libraries to preload into each backend."),
            NULL,
            GUC_LIST_INPUT | GUC_LIST_QUOTE | GUC_SUPERUSER_ONLY
        },
        &session_preload_libraries_string,
        "",
        NULL, NULL, NULL
    },

    {
        {"shared_preload_libraries", PGC_POSTMASTER, CLIENT_CONN_PRELOAD,
            gettext_noop("Lists shared libraries to preload into server."),
            NULL,
            GUC_LIST_INPUT | GUC_LIST_QUOTE | GUC_SUPERUSER_ONLY
        },
        &shared_preload_libraries_string,
        "",
        NULL, NULL, NULL
    },

    {
        {"local_preload_libraries", PGC_USERSET, CLIENT_CONN_PRELOAD,
            gettext_noop("Lists unprivileged shared libraries to preload into each backend."),
            NULL,
            GUC_LIST_INPUT | GUC_LIST_QUOTE
        },
        &local_preload_libraries_string,
        "",
        NULL, NULL, NULL
    },

    {
        {"search_path", PGC_USERSET, CLIENT_CONN_STATEMENT,
            gettext_noop("Sets the schema search order for names that are not schema-qualified."),
            NULL,
            GUC_LIST_INPUT | GUC_LIST_QUOTE
        },
        &namespace_search_path,
        "\"$user\", public",
        check_search_path, assign_search_path, NULL
    },

    {
        /* Can't be set in postgresql.conf */
        {"server_encoding", PGC_INTERNAL, CLIENT_CONN_LOCALE,
            gettext_noop("Sets the server (database) character set encoding."),
            NULL,
            GUC_IS_NAME | GUC_REPORT | GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
        },
        &server_encoding_string,
        "SQL_ASCII",
        NULL, NULL, NULL
    },

    {
        /* Can't be set in postgresql.conf */
        {"server_version", PGC_INTERNAL, PRESET_OPTIONS,
            gettext_noop("Shows the server version."),
            NULL,
            GUC_REPORT | GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
        },
        &server_version_string,
        PG_VERSION,
        NULL, NULL, NULL
    },

    {
        /* Not for general use --- used by SET ROLE */
        {"role", PGC_USERSET, UNGROUPED,
            gettext_noop("Sets the current role."),
            NULL,
            GUC_IS_NAME | GUC_NO_SHOW_ALL | GUC_NO_RESET_ALL | GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE | GUC_NOT_WHILE_SEC_REST
        },
        &role_string,
        "none",
        check_role, assign_role, show_role
    },

    {
        /* Not for general use --- used by SET SESSION AUTHORIZATION */
        {"session_authorization", PGC_USERSET, UNGROUPED,
            gettext_noop("Sets the session user name."),
            NULL,
            GUC_IS_NAME | GUC_REPORT | GUC_NO_SHOW_ALL | GUC_NO_RESET_ALL | GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE | GUC_NOT_WHILE_SEC_REST
        },
        &session_authorization_string,
        NULL,
        check_session_authorization, assign_session_authorization, NULL
    },

#ifdef XCP
    {
        {"global_session", PGC_USERSET, UNGROUPED,
            gettext_noop("Sets the global session identifier."),
            NULL,
            GUC_IS_NAME | GUC_REPORT | GUC_NO_RESET_ALL | GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE | GUC_NOT_WHILE_SEC_REST
        },
        &global_session_string,
        "none",
        check_global_session, assign_global_session, NULL
    },
#endif

    {
        {"log_destination", PGC_SIGHUP, LOGGING_WHERE,
            gettext_noop("Sets the destination for server log output."),
            gettext_noop("Valid values are combinations of \"stderr\", "
                         "\"syslog\", \"csvlog\", and \"eventlog\", "
                         "depending on the platform."),
            GUC_LIST_INPUT
        },
        &Log_destination_string,
        "stderr",
        check_log_destination, assign_log_destination, NULL
    },
    {
        {"log_directory", PGC_SIGHUP, LOGGING_WHERE,
            gettext_noop("Sets the destination directory for log files."),
            gettext_noop("Can be specified as relative to the data directory "
                         "or as absolute path."),
            GUC_SUPERUSER_ONLY
        },
        &Log_directory,
        "log",
        check_canonical_path, NULL, NULL
    },
    {
        {"log_filename", PGC_SIGHUP, LOGGING_WHERE,
            gettext_noop("Sets the file name pattern for log files."),
            NULL,
            GUC_SUPERUSER_ONLY
        },
        &Log_filename,
        "postgresql-%Y-%m-%d_%H%M%S.log",
        NULL, NULL, NULL
    },
#ifdef __AUDIT__
    {
        {"alog_filename", PGC_SIGHUP, LOGGING_WHERE,
            gettext_noop("Sets the file name pattern for audit log files."),
            NULL,
            GUC_SUPERUSER_ONLY
        },
        &AuditLog_filename,
        "audit-%A-%H.log",
        NULL, NULL, NULL
    },
#endif
    {
        {"syslog_ident", PGC_SIGHUP, LOGGING_WHERE,
            gettext_noop("Sets the program name used to identify PostgreSQL "
                         "messages in syslog."),
            NULL
        },
        &syslog_ident_str,
        "postgres",
        NULL, assign_syslog_ident, NULL
    },

    {
        {"event_source", PGC_POSTMASTER, LOGGING_WHERE,
            gettext_noop("Sets the application name used to identify "
                         "PostgreSQL messages in the event log."),
            NULL
        },
        &event_source,
        DEFAULT_EVENT_SOURCE,
        NULL, NULL, NULL
    },

    {
        {"TimeZone", PGC_USERSET, CLIENT_CONN_LOCALE,
            gettext_noop("Sets the time zone for displaying and interpreting time stamps."),
            NULL,
            GUC_REPORT
        },
        &timezone_string,
        "GMT",
        check_timezone, assign_timezone, show_timezone
    },
    {
        {"timezone_abbreviations", PGC_USERSET, CLIENT_CONN_LOCALE,
            gettext_noop("Selects a file of time zone abbreviations."),
            NULL
        },
        &timezone_abbreviations_string,
        NULL,
        check_timezone_abbreviations, assign_timezone_abbreviations, NULL
    },

    {
        {"transaction_isolation", PGC_USERSET, CLIENT_CONN_STATEMENT,
            gettext_noop("Sets the current transaction's isolation level."),
            NULL,
            GUC_NO_RESET_ALL | GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
        },
        &XactIsoLevel_string,
        "default",
        check_XactIsoLevel, assign_XactIsoLevel, show_XactIsoLevel
    },

    {
        {"unix_socket_group", PGC_POSTMASTER, CONN_AUTH_SETTINGS,
            gettext_noop("Sets the owning group of the Unix-domain socket."),
            gettext_noop("The owning user of the socket is always the user "
                         "that starts the server.")
        },
        &Unix_socket_group,
        "",
        NULL, NULL, NULL
    },

    {
        {"unix_socket_directories", PGC_POSTMASTER, CONN_AUTH_SETTINGS,
            gettext_noop("Sets the directories where Unix-domain sockets will be created."),
            NULL,
            GUC_SUPERUSER_ONLY
        },
        &Unix_socket_directories,
#ifdef HAVE_UNIX_SOCKETS
        DEFAULT_PGSOCKET_DIR,
#else
        "",
#endif
        NULL, NULL, NULL
    },

    {
        {"listen_addresses", PGC_POSTMASTER, CONN_AUTH_SETTINGS,
            gettext_noop("Sets the host name or IP address(es) to listen to."),
            NULL,
            GUC_LIST_INPUT
        },
        &ListenAddresses,
        "localhost",
        NULL, NULL, NULL
    },

    {
        /*
         * Can't be set by ALTER SYSTEM as it can lead to recursive definition
         * of data_directory.
         */
        {"data_directory", PGC_POSTMASTER, FILE_LOCATIONS,
            gettext_noop("Sets the server's data directory."),
            NULL,
            GUC_SUPERUSER_ONLY | GUC_DISALLOW_IN_AUTO_FILE
        },
        &data_directory,
        NULL,
        NULL, NULL, NULL
    },

    {
        {"config_file", PGC_POSTMASTER, FILE_LOCATIONS,
            gettext_noop("Sets the server's main configuration file."),
            NULL,
            GUC_DISALLOW_IN_FILE | GUC_SUPERUSER_ONLY
        },
        &ConfigFileName,
        NULL,
        NULL, NULL, NULL
    },

    {
        {"hba_file", PGC_POSTMASTER, FILE_LOCATIONS,
            gettext_noop("Sets the server's \"hba\" configuration file."),
            NULL,
            GUC_SUPERUSER_ONLY
        },
        &HbaFileName,
        NULL,
        NULL, NULL, NULL
    },

    {
        {"ident_file", PGC_POSTMASTER, FILE_LOCATIONS,
            gettext_noop("Sets the server's \"ident\" configuration file."),
            NULL,
            GUC_SUPERUSER_ONLY
        },
        &IdentFileName,
        NULL,
        NULL, NULL, NULL
    },

    {
        {"external_pid_file", PGC_POSTMASTER, FILE_LOCATIONS,
            gettext_noop("Writes the postmaster PID to the specified file."),
            NULL,
            GUC_SUPERUSER_ONLY
        },
        &external_pid_file,
        NULL,
        check_canonical_path, NULL, NULL
    },

    {
        {"ssl_cert_file", PGC_SIGHUP, CONN_AUTH_SECURITY,
            gettext_noop("Location of the SSL server certificate file."),
            NULL
        },
        &ssl_cert_file,
        "server.crt",
        NULL, NULL, NULL
    },

    {
        {"ssl_key_file", PGC_SIGHUP, CONN_AUTH_SECURITY,
            gettext_noop("Location of the SSL server private key file."),
            NULL
        },
        &ssl_key_file,
        "server.key",
        NULL, NULL, NULL
    },

    {
        {"ssl_ca_file", PGC_SIGHUP, CONN_AUTH_SECURITY,
            gettext_noop("Location of the SSL certificate authority file."),
            NULL
        },
        &ssl_ca_file,
        "",
        NULL, NULL, NULL
    },

    {
        {"ssl_crl_file", PGC_SIGHUP, CONN_AUTH_SECURITY,
            gettext_noop("Location of the SSL certificate revocation list file."),
            NULL
        },
        &ssl_crl_file,
        "",
        NULL, NULL, NULL
    },

    {
        {"stats_temp_directory", PGC_SIGHUP, STATS_COLLECTOR,
            gettext_noop("Writes temporary statistics files to the specified directory."),
            NULL,
            GUC_SUPERUSER_ONLY
        },
        &pgstat_temp_directory,
        PG_STAT_TMP_DIR,
        check_canonical_path, assign_pgstat_temp_directory, NULL
    },

    {
        {"synchronous_standby_names", PGC_SIGHUP, REPLICATION_MASTER,
            gettext_noop("Number of synchronous standbys and list of names of potential synchronous ones."),
            NULL,
            GUC_LIST_INPUT
        },
        &SyncRepStandbyNames,
        "",
        check_synchronous_standby_names, assign_synchronous_standby_names, NULL
    },

    {
        {"default_text_search_config", PGC_USERSET, CLIENT_CONN_LOCALE,
            gettext_noop("Sets default text search configuration."),
            NULL
        },
        &TSCurrentConfig,
        "pg_catalog.simple",
        check_TSCurrentConfig, assign_TSCurrentConfig, NULL
    },

#ifdef PGXC
    {
        {"pgxc_node_name", PGC_POSTMASTER, GTM,
            gettext_noop("The Coordinator or Datanode name."),
            NULL,
            GUC_NO_RESET_ALL | GUC_IS_NAME
        },
        &PGXCNodeName,
        "",
        NULL, NULL, NULL
    },

    {
		{"pgxc_cluster_name", PGC_SIGHUP, GTM,
            gettext_noop("The Cluster name."),
            NULL,
            GUC_NO_RESET_ALL | GUC_IS_NAME
        },
        &PGXCClusterName,
        "tbase_cluster",
        NULL, NULL, NULL
    },
    {
        {"pgxc_main_cluster_name", PGC_POSTMASTER, GTM,
            gettext_noop("The Main Cluster name."),
            NULL,
            GUC_NO_RESET_ALL | GUC_IS_NAME
        },
        &PGXCMainClusterName,
        "tbase_cluster",
        NULL, NULL, NULL
    },
#endif
#ifdef XCP
    {
        {"parentnode", PGC_BACKEND, CONN_AUTH,
            gettext_noop("Sets the name of the parent data node"),
            NULL
        },
        &parentPGXCNode,
        NULL,
        NULL, NULL, NULL
    },
#endif /* XCP */
    {
        {"ssl_ciphers", PGC_SIGHUP, CONN_AUTH_SECURITY,
            gettext_noop("Sets the list of allowed SSL ciphers."),
            NULL,
            GUC_SUPERUSER_ONLY
        },
        &SSLCipherSuites,
#ifdef USE_SSL
        "HIGH:MEDIUM:+3DES:!aNULL",
#else
        "none",
#endif
        NULL, NULL, NULL
    },

    {
        {"ssl_ecdh_curve", PGC_SIGHUP, CONN_AUTH_SECURITY,
            gettext_noop("Sets the curve to use for ECDH."),
            NULL,
            GUC_SUPERUSER_ONLY
        },
        &SSLECDHCurve,
#ifdef USE_SSL
        "prime256v1",
#else
        "none",
#endif
        NULL, NULL, NULL
    },

    {
        {"ssl_dh_params_file", PGC_SIGHUP, CONN_AUTH_SECURITY,
            gettext_noop("Location of the SSL DH params file."),
            NULL,
            GUC_SUPERUSER_ONLY
        },
        &ssl_dh_params_file,
        "",
        NULL, NULL, NULL
    },

    {
        {"application_name", PGC_USERSET, LOGGING_WHAT,
            gettext_noop("Sets the application name to be reported in statistics and logs."),
            NULL,
            GUC_IS_NAME | GUC_REPORT | GUC_NOT_IN_SAMPLE
        },
        &application_name,
        "",
        check_application_name, assign_application_name, NULL
    },

    {
        {"cluster_name", PGC_POSTMASTER, PROCESS_TITLE,
            gettext_noop("Sets the name of the cluster, which is included in the process title."),
            NULL,
            GUC_IS_NAME
        },
        &cluster_name,
        "",
        check_cluster_name, NULL, NULL
    },

    {
        {"wal_consistency_checking", PGC_SUSET, DEVELOPER_OPTIONS,
            gettext_noop("Sets the WAL resource managers for which WAL consistency checks are done."),
            gettext_noop("Full-page images will be logged for all data blocks and cross-checked against the results of WAL replay."),
            GUC_LIST_INPUT | GUC_NOT_IN_SAMPLE
        },
        &wal_consistency_checking_string,
        "",
        check_wal_consistency_checking, assign_wal_consistency_checking, NULL
    },
#ifdef __TBASE__
    {
        {"pgbouncer_conf", PGC_POSTMASTER, PROCESS_TITLE,
            gettext_noop("pgbouncer config file name, in PGDATA dir."),
            NULL,
            GUC_IS_NAME
        },
        &g_BouncerConf,
        "pgbouncer.ini",
        NULL, NULL, NULL
    },
#endif
#ifdef __TBASE__
    {
        {"pooler_warm_db_user", PGC_USERSET, CUSTOM_OPTIONS,
            gettext_noop("pooler warm datanode database and user."),
            NULL
        },
        &g_PoolerWarmBufferInfo,
        "",
        NULL, NULL, NULL
    },
    {
        {"pooler_unpooled_database", PGC_USERSET, CUSTOM_OPTIONS,
            gettext_noop("databases that pooler will not pool its connections."),
            NULL
        },
        &g_unpooled_database,
        "template1",
        NULL, NULL, NULL
    },
    {
        {"pooler_unpooled_user", PGC_USERSET, CUSTOM_OPTIONS,
            gettext_noop("users that pooler will not pool its connections."),
            NULL
        },
        &g_unpooled_user,
        "mls_admin",
        NULL, NULL, NULL
    },
#endif
#ifdef _PG_ORCL_
    {
        {"nls_date_format", PGC_USERSET, CUSTOM_OPTIONS,
            gettext_noop("Emulate oracle's date output behaviour."),
            NULL,
            GUC_REPORT | GUC_NOT_IN_SAMPLE
        },
        &nls_date_format,
        "YYYY-MM-DD HH24:MI:SS",
        NULL, NULL, NULL
    },

    {
        {"nls_timestamp_format", PGC_USERSET, CUSTOM_OPTIONS,
            gettext_noop("Emulate oracle's timestamp without time zone output behaviour."),
            NULL,
            GUC_REPORT | GUC_NOT_IN_SAMPLE
        },
        &nls_timestamp_format,
        "YYYY-MM-DD HH24:MI:SS.US",
        NULL, NULL, NULL
    },

    {
        {"nls_timestamp_tz_format", PGC_USERSET, CUSTOM_OPTIONS,
            gettext_noop("Emulate oracle's timestamp with time zone output behaviour."),
            NULL,
            GUC_REPORT | GUC_NOT_IN_SAMPLE
        },
        &nls_timestamp_tz_format,
        "YYYY-MM-DD HH24:MI:SS.US TZ",
        NULL, NULL, NULL
    },

    {
        {"nls_sort_locale", PGC_USERSET, CUSTOM_OPTIONS,
            gettext_noop("COLLATE set for nlssort."),
            NULL,
            GUC_REPORT | GUC_NOT_IN_SAMPLE
        },
        &nls_sort_locale,
        NULL,
        NULL, NULL, NULL
    },
#endif
#ifdef __COLD_HOT__
    {
        {"cold_hot_sepration_mode", PGC_POSTMASTER, RESOURCES_KERNEL,
            gettext_noop("Set the cold_hot_sepration_mode, day/month/year."),
            NULL,
            GUC_IS_NAME |  GUC_NOT_IN_SAMPLE
        },
        &g_ColdHotPartitionMode,
        "month",
        NULL, assign_cold_hot_partition_type, NULL
    },
    {
        {"manual_hot_date", PGC_SIGHUP, RESOURCES_KERNEL,
            gettext_noop("Set the manual_hot_date, this value will mask off hot_date_age."),
            NULL,
            GUC_IS_NAME |  GUC_NOT_IN_SAMPLE
        },
        &g_ManualHotDate,
        "",
        NULL, assign_manual_hot_date, NULL
    },
    {
        {"temp_cold_date", PGC_USERSET, LOGGING_WHAT,
            gettext_noop("Set the temp cold date, so the data import soon will be trade as cold data."),
            NULL,
            GUC_IS_NAME |  GUC_NOT_IN_SAMPLE
        },
        &g_TempColdDate,
        "",
        NULL, assign_template_cold_date, NULL
    },
    {
        {"temp_key_value", PGC_USERSET, LOGGING_WHAT,
            gettext_noop("Set the temp key value, so the white list tuple will be routed to new group."),
            NULL,
            GUC_IS_NAME |  GUC_NOT_IN_SAMPLE
        },
        &g_TempKeyValue,
        "",
        NULL, SetTempKeyValueList, NULL
    },
    {
        {"temp_hot_date", PGC_USERSET, LOGGING_WHAT,
            gettext_noop("Set the temp hot date, when check data consistency in code datanode."),
            NULL,
            GUC_IS_NAME |  GUC_NOT_IN_SAMPLE
        },
        &g_TempHotDate,
        "",
        NULL, assign_template_hot_date, NULL
    },
#endif
#ifdef _MLS_
    {
        {"default_locator_type", PGC_USERSET, LOGGING_WHAT,
            gettext_noop("set default table locator type, enum as shard/hash/replication, default is empty string"),
            NULL,
            GUC_IS_NAME |  GUC_NOT_IN_SAMPLE
        },
        &g_default_locator_type,
        "",
        NULL, assign_default_locator_type, NULL
    },
#endif
#ifdef _PUB_SUB_RELIABLE_
    {
        {"wal_stream_type", PGC_USERSET, WAL_SETTINGS,
            gettext_noop("set wal_stream_type type, enum as user_stream/cluster_stream/internal_stream, default is user_stream string"),
            NULL,
            GUC_NOT_IN_SAMPLE
        },
        &g_wal_stream_type_str,
        "user_stream",
        NULL, assign_wal_stream_type, NULL
    },
#endif    
    /* End-of-list marker */
    {
        {NULL, 0, 0, NULL, NULL}, NULL, NULL, NULL, NULL, NULL
    }
};


static struct config_enum ConfigureNamesEnum[] =
{
    {
        {"backslash_quote", PGC_USERSET, COMPAT_OPTIONS_PREVIOUS,
            gettext_noop("Sets whether \"\\'\" is allowed in string literals."),
            NULL
        },
        &backslash_quote,
        BACKSLASH_QUOTE_SAFE_ENCODING, backslash_quote_options,
        NULL, NULL, NULL
    },

    {
        {"bytea_output", PGC_USERSET, CLIENT_CONN_STATEMENT,
            gettext_noop("Sets the output format for bytea."),
            NULL
        },
        &bytea_output,
        BYTEA_OUTPUT_HEX, bytea_output_options,
        NULL, NULL, NULL
    },

    {
        {"client_min_messages", PGC_USERSET, LOGGING_WHEN,
            gettext_noop("Sets the message levels that are sent to the client."),
            gettext_noop("Each level includes all the levels that follow it. The later"
                         " the level, the fewer messages are sent.")
        },
        &client_min_messages,
        NOTICE, client_message_level_options,
        NULL, NULL, NULL
    },

    {
        {"constraint_exclusion", PGC_USERSET, QUERY_TUNING_OTHER,
            gettext_noop("Enables the planner to use constraints to optimize queries."),
            gettext_noop("Table scans will be skipped if their constraints"
                         " guarantee that no rows match the query.")
        },
        &constraint_exclusion,
        CONSTRAINT_EXCLUSION_PARTITION, constraint_exclusion_options,
        NULL, NULL, NULL
    },

    {
        {"default_transaction_isolation", PGC_USERSET, CLIENT_CONN_STATEMENT,
            gettext_noop("Sets the transaction isolation level of each new transaction."),
            NULL
        },
        &DefaultXactIsoLevel,
        XACT_READ_COMMITTED, isolation_level_options,
        NULL, NULL, NULL
    },

    {
        {"IntervalStyle", PGC_USERSET, CLIENT_CONN_LOCALE,
            gettext_noop("Sets the display format for interval values."),
            NULL,
            GUC_REPORT
        },
        &IntervalStyle,
        INTSTYLE_POSTGRES, intervalstyle_options,
        NULL, NULL, NULL
    },

    {
        {"log_error_verbosity", PGC_SUSET, LOGGING_WHAT,
            gettext_noop("Sets the verbosity of logged messages."),
            NULL
        },
        &Log_error_verbosity,
        PGERROR_DEFAULT, log_error_verbosity_options,
        NULL, NULL, NULL
    },

    {
        {"log_min_messages", PGC_SUSET, LOGGING_WHEN,
            gettext_noop("Sets the message levels that are logged."),
            gettext_noop("Each level includes all the levels that follow it. The later"
                         " the level, the fewer messages are sent.")
        },
        &log_min_messages,
        WARNING, server_message_level_options,
        NULL, NULL, NULL
    },

    {
        {"log_min_error_statement", PGC_SUSET, LOGGING_WHEN,
            gettext_noop("Causes all statements generating error at or above this level to be logged."),
            gettext_noop("Each level includes all the levels that follow it. The later"
                         " the level, the fewer messages are sent.")
        },
        &log_min_error_statement,
        ERROR, server_message_level_options,
        NULL, NULL, NULL
    },

    {
        {"log_statement", PGC_SUSET, LOGGING_WHAT,
            gettext_noop("Sets the type of statements logged."),
            NULL
        },
        &log_statement,
        LOGSTMT_NONE, log_statement_options,
        NULL, NULL, NULL
    },

    {
        {"syslog_facility", PGC_SIGHUP, LOGGING_WHERE,
            gettext_noop("Sets the syslog \"facility\" to be used when syslog enabled."),
            NULL
        },
        &syslog_facility,
#ifdef HAVE_SYSLOG
        LOG_LOCAL0,
#else
        0,
#endif
        syslog_facility_options,
        NULL, assign_syslog_facility, NULL
    },

    {
        {"session_replication_role", PGC_SUSET, CLIENT_CONN_STATEMENT,
            gettext_noop("Sets the session's behavior for triggers and rewrite rules."),
            NULL
        },
        &SessionReplicationRole,
        SESSION_REPLICATION_ROLE_ORIGIN, session_replication_role_options,
        NULL, assign_session_replication_role, NULL
    },

    {
        {"synchronous_commit", PGC_USERSET, WAL_SETTINGS,
            gettext_noop("Sets the current transaction's synchronization level."),
            NULL
        },
        &synchronous_commit,
        SYNCHRONOUS_COMMIT_ON, synchronous_commit_options,
        NULL, assign_synchronous_commit, NULL
    },

    {
        {"archive_mode", PGC_POSTMASTER, WAL_ARCHIVING,
            gettext_noop("Allows archiving of WAL files using archive_command."),
            NULL
        },
        &XLogArchiveMode,
        ARCHIVE_MODE_OFF, archive_mode_options,
        NULL, NULL, NULL
    },

    {
        {"trace_recovery_messages", PGC_SIGHUP, DEVELOPER_OPTIONS,
            gettext_noop("Enables logging of recovery-related debugging information."),
            gettext_noop("Each level includes all the levels that follow it. The later"
                         " the level, the fewer messages are sent.")
        },
        &trace_recovery_messages,

        /*
         * client_message_level_options allows too many values, really, but
         * it's not worth having a separate options array for this.
         */
        LOG, client_message_level_options,
        NULL, NULL, NULL
    },

    {
        {"track_functions", PGC_SUSET, STATS_COLLECTOR,
            gettext_noop("Collects function-level statistics on database activity."),
            NULL
        },
        &pgstat_track_functions,
        TRACK_FUNC_OFF, track_function_options,
        NULL, NULL, NULL
    },

    {
        {"wal_level", PGC_POSTMASTER, WAL_SETTINGS,
            gettext_noop("Set the level of information written to the WAL."),
            NULL
        },
        &wal_level,
        WAL_LEVEL_LOGICAL, wal_level_options,
        NULL, NULL, NULL
    },

    {
        {"dynamic_shared_memory_type", PGC_POSTMASTER, RESOURCES_MEM,
            gettext_noop("Selects the dynamic shared memory implementation used."),
            NULL
        },
        &dynamic_shared_memory_type,
        DEFAULT_DYNAMIC_SHARED_MEMORY_TYPE, dynamic_shared_memory_options,
        NULL, NULL, NULL
    },

    {
        {"wal_sync_method", PGC_SIGHUP, WAL_SETTINGS,
            gettext_noop("Selects the method used for forcing WAL updates to disk."),
            NULL
        },
        &sync_method,
        DEFAULT_SYNC_METHOD, sync_method_options,
        NULL, assign_xlog_sync_method, NULL
    },

    {
        {"xmlbinary", PGC_USERSET, CLIENT_CONN_STATEMENT,
            gettext_noop("Sets how binary values are to be encoded in XML."),
            NULL
        },
        &xmlbinary,
        XMLBINARY_BASE64, xmlbinary_options,
        NULL, NULL, NULL
    },

    {
        {"xmloption", PGC_USERSET, CLIENT_CONN_STATEMENT,
            gettext_noop("Sets whether XML data in implicit parsing and serialization "
                         "operations is to be considered as documents or content fragments."),
            NULL
        },
        &xmloption,
        XMLOPTION_CONTENT, xmloption_options,
        NULL, NULL, NULL
    },

#ifdef PGXC
    {
		{"remotetype", PGC_USERSET, CONN_AUTH,
            gettext_noop("Sets the type of Postgres-XL remote connection"),
            NULL
        },
        &remoteConnType,
        REMOTE_CONN_APP, pgxc_conn_types,
        NULL, NULL, NULL
    },
#endif
    {
        {"huge_pages", PGC_POSTMASTER, RESOURCES_MEM,
            gettext_noop("Use of huge pages on Linux."),
            NULL
        },
        &huge_pages,
        HUGE_PAGES_TRY, huge_pages_options,
        NULL, NULL, NULL
    },

#ifdef XCP
    {
        {"global_snapshot_source", PGC_USERSET, DEVELOPER_OPTIONS,
            gettext_noop("Set preferred source of a snapshot."),
            gettext_noop("When set to 'coordinator', a snapshot is taken at "
                    "the coordinator at the risk of reduced consistency. "
                    "Default is 'gtm'")
        },
        &GlobalSnapshotSource,
        GLOBAL_SNAPSHOT_SOURCE_GTM, global_snapshot_source_options,
        NULL, NULL, NULL
    },
#endif
#ifdef _SHARDING_
    {
        {"shard_visible_mode", PGC_USERSET, SHARD_VISIBLE_MODE,
            gettext_noop("set the mode of shard visibility."),
            NULL
        },
        &g_ShardVisibleMode,
        SHARD_VISIBLE_MODE_VISIBLE, shard_visible_modes,
        NULL, NULL, NULL
    },
#endif
    {
        {"force_parallel_mode", PGC_USERSET, QUERY_TUNING_OTHER,
            gettext_noop("Forces use of parallel query facilities."),
            gettext_noop("If possible, run query using a parallel worker and with parallel restrictions.")
        },
        &force_parallel_mode,
        FORCE_PARALLEL_OFF, force_parallel_mode_options,
        NULL, NULL, NULL
    },

    {
        {"password_encryption", PGC_USERSET, CONN_AUTH_SECURITY,
            gettext_noop("Encrypt passwords."),
            gettext_noop("When a password is specified in CREATE USER or "
                         "ALTER USER without writing either ENCRYPTED or UNENCRYPTED, "
                         "this parameter determines whether the password is to be encrypted.")
        },
        &Password_encryption,
        PASSWORD_TYPE_MD5, password_encryption_options,
        NULL, NULL, NULL
    },
#ifdef __TBASE__
    {
        {"archive_status_control", PGC_USERSET, WAL_ARCHIVING,
            gettext_noop("Control of break or continue archive xlog. "),
            NULL
        },
        &archive_status_control,
        ARCHSTATUS_CONTINUE, archive_status_control_options,
        NULL, NULL, NULL
    },
#endif
    /* End-of-list marker */
    {
        {NULL, 0, 0, NULL, NULL}, NULL, 0, NULL, NULL, NULL, NULL
    }
};

/******** end of options list ********/


/*
 * To allow continued support of obsolete names for GUC variables, we apply
 * the following mappings to any unrecognized name.  Note that an old name
 * should be mapped to a new one only if the new variable has very similar
 * semantics to the old.
 */
static const char *const map_old_guc_names[] = {
    "sort_mem", "work_mem",
    "vacuum_mem", "maintenance_work_mem",
    NULL
};


/*
 * Actual lookup of variables is done through this single, sorted array.
 */
static struct config_generic **guc_variables;

/* Current number of variables contained in the vector */
static int    num_guc_variables;

/* Vector capacity */
static int    size_guc_variables;


static bool guc_dirty;            /* TRUE if need to do commit/abort work */

static bool reporting_enabled;    /* TRUE to enable GUC_REPORT */

static int    GUCNestLevel = 0;    /* 1 when in main transaction */


static int    guc_var_compare(const void *a, const void *b);
static int    guc_name_compare(const char *namea, const char *nameb);
static void InitializeGUCOptionsFromEnvironment(void);
static void InitializeOneGUCOption(struct config_generic *gconf);
static void push_old_value(struct config_generic *gconf, GucAction action);
static void ReportGUCOption(struct config_generic *record);
static void reapply_stacked_values(struct config_generic *variable,
                       struct config_string *pHolder,
                       GucStack *stack,
                       const char *curvalue,
                       GucContext curscontext, GucSource cursource);
static void ShowGUCConfigOption(const char *name, DestReceiver *dest);
static void ShowAllGUCConfig(DestReceiver *dest);
static char *_ShowOption(struct config_generic *record, bool use_units);
static bool validate_option_array_item(const char *name, const char *value,
                           bool skipIfNoPermissions);
static void write_auto_conf_file(int fd, const char *filename, ConfigVariable *head_p);
static void replace_auto_config_value(ConfigVariable **head_p, ConfigVariable **tail_p,
                          const char *name, const char *value);


/*
 * Some infrastructure for checking malloc/strdup/realloc calls
 */
static void *
guc_malloc(int elevel, size_t size)
{
    void       *data;

    /* Avoid unportable behavior of malloc(0) */
    if (size == 0)
        size = 1;
    data = malloc(size);
    if (data == NULL)
        ereport(elevel,
                (errcode(ERRCODE_OUT_OF_MEMORY),
                 errmsg("out of memory")));
    return data;
}

static void *
guc_realloc(int elevel, void *old, size_t size)
{
    void       *data;

    /* Avoid unportable behavior of realloc(NULL, 0) */
    if (old == NULL && size == 0)
        size = 1;
    data = realloc(old, size);
    if (data == NULL)
        ereport(elevel,
                (errcode(ERRCODE_OUT_OF_MEMORY),
                 errmsg("out of memory")));
    return data;
}

static char *
guc_strdup(int elevel, const char *src)
{
    char       *data;

    data = strdup(src);
    if (data == NULL)
        ereport(elevel,
                (errcode(ERRCODE_OUT_OF_MEMORY),
                 errmsg("out of memory")));
    return data;
}


/*
 * Detect whether strval is referenced anywhere in a GUC string item
 */
static bool
string_field_used(struct config_string *conf, char *strval)
{
    GucStack   *stack;

    if (strval == *(conf->variable) ||
        strval == conf->reset_val ||
        strval == conf->boot_val)
        return true;
    for (stack = conf->gen.stack; stack; stack = stack->prev)
    {
        if (strval == stack->prior.val.stringval ||
            strval == stack->masked.val.stringval)
            return true;
    }
    return false;
}

/*
 * Support for assigning to a field of a string GUC item.  Free the prior
 * value if it's not referenced anywhere else in the item (including stacked
 * states).
 */
static void
set_string_field(struct config_string *conf, char **field, char *newval)
{
    char       *oldval = *field;

    /* Do the assignment */
    *field = newval;

    /* Free old value if it's not NULL and isn't referenced anymore */
    if (oldval && !string_field_used(conf, oldval))
        free(oldval);
}

/*
 * Detect whether an "extra" struct is referenced anywhere in a GUC item
 */
static bool
extra_field_used(struct config_generic *gconf, void *extra)
{// #lizard forgives
    GucStack   *stack;

    if (extra == gconf->extra)
        return true;
    switch (gconf->vartype)
    {
        case PGC_BOOL:
            if (extra == ((struct config_bool *) gconf)->reset_extra)
                return true;
            break;
        case PGC_INT:
            if (extra == ((struct config_int *) gconf)->reset_extra)
                return true;
            break;
        case PGC_UINT:
            if (extra == ((struct config_uint *) gconf)->reset_extra)
                return true;
            break;
        case PGC_REAL:
            if (extra == ((struct config_real *) gconf)->reset_extra)
                return true;
            break;
        case PGC_STRING:
            if (extra == ((struct config_string *) gconf)->reset_extra)
                return true;
            break;
        case PGC_ENUM:
            if (extra == ((struct config_enum *) gconf)->reset_extra)
                return true;
            break;
    }
    for (stack = gconf->stack; stack; stack = stack->prev)
    {
        if (extra == stack->prior.extra ||
            extra == stack->masked.extra)
            return true;
    }

    return false;
}

/*
 * Support for assigning to an "extra" field of a GUC item.  Free the prior
 * value if it's not referenced anywhere else in the item (including stacked
 * states).
 */
static void
set_extra_field(struct config_generic *gconf, void **field, void *newval)
{
    void       *oldval = *field;

    /* Do the assignment */
    *field = newval;

    /* Free old value if it's not NULL and isn't referenced anymore */
    if (oldval && !extra_field_used(gconf, oldval))
        free(oldval);
}

/*
 * Support for copying a variable's active value into a stack entry.
 * The "extra" field associated with the active value is copied, too.
 *
 * NB: be sure stringval and extra fields of a new stack entry are
 * initialized to NULL before this is used, else we'll try to free() them.
 */
static void
set_stack_value(struct config_generic *gconf, config_var_value *val)
{
    switch (gconf->vartype)
    {
        case PGC_BOOL:
            val->val.boolval =
                *((struct config_bool *) gconf)->variable;
            break;
        case PGC_INT:
            val->val.intval =
                *((struct config_int *) gconf)->variable;
            break;
        case PGC_UINT:
            val->val.intval =
                *((struct config_uint *) gconf)->variable;
            break;
        case PGC_REAL:
            val->val.realval =
                *((struct config_real *) gconf)->variable;
            break;
        case PGC_STRING:
            set_string_field((struct config_string *) gconf,
                             &(val->val.stringval),
                             *((struct config_string *) gconf)->variable);
            break;
        case PGC_ENUM:
            val->val.enumval =
                *((struct config_enum *) gconf)->variable;
            break;
    }
    set_extra_field(gconf, &(val->extra), gconf->extra);
}

/*
 * Support for discarding a no-longer-needed value in a stack entry.
 * The "extra" field associated with the stack entry is cleared, too.
 */
static void
discard_stack_value(struct config_generic *gconf, config_var_value *val)
{
    switch (gconf->vartype)
    {
        case PGC_BOOL:
        case PGC_INT:
        case PGC_UINT:
        case PGC_REAL:
        case PGC_ENUM:
            /* no need to do anything */
            break;
        case PGC_STRING:
            set_string_field((struct config_string *) gconf,
                             &(val->val.stringval),
                             NULL);
            break;
    }
    set_extra_field(gconf, &(val->extra), NULL);
}


/*
 * Fetch the sorted array pointer (exported for help_config.c's use ONLY)
 */
struct config_generic **
get_guc_variables(void)
{
    return guc_variables;
}


/*
 * Build the sorted array.  This is split out so that it could be
 * re-executed after startup (e.g., we could allow loadable modules to
 * add vars, and then we'd need to re-sort).
 */
void
build_guc_variables(void)
{// #lizard forgives
    int            size_vars;
    int            num_vars = 0;
    struct config_generic **guc_vars;
    int            i;

    for (i = 0; ConfigureNamesBool[i].gen.name; i++)
    {
        struct config_bool *conf = &ConfigureNamesBool[i];

        /* Rather than requiring vartype to be filled in by hand, do this: */
        conf->gen.vartype = PGC_BOOL;
        num_vars++;
    }

    for (i = 0; ConfigureNamesInt[i].gen.name; i++)
    {
        struct config_int *conf = &ConfigureNamesInt[i];

        conf->gen.vartype = PGC_INT;
        num_vars++;
    }

    for (i = 0; ConfigureNamesUInt[i].gen.name; i++)
    {
        struct config_uint *conf = &ConfigureNamesUInt[i];

        conf->gen.vartype = PGC_UINT;
        num_vars++;
    }

    for (i = 0; ConfigureNamesReal[i].gen.name; i++)
    {
        struct config_real *conf = &ConfigureNamesReal[i];

        conf->gen.vartype = PGC_REAL;
        num_vars++;
    }

    for (i = 0; ConfigureNamesString[i].gen.name; i++)
    {
        struct config_string *conf = &ConfigureNamesString[i];

        conf->gen.vartype = PGC_STRING;
        num_vars++;
    }

    for (i = 0; ConfigureNamesEnum[i].gen.name; i++)
    {
        struct config_enum *conf = &ConfigureNamesEnum[i];

        conf->gen.vartype = PGC_ENUM;
        num_vars++;
    }

    /*
     * Create table with 20% slack
     */
    size_vars = num_vars + num_vars / 4;

    guc_vars = (struct config_generic **)
        guc_malloc(FATAL, size_vars * sizeof(struct config_generic *));

    num_vars = 0;

    for (i = 0; ConfigureNamesBool[i].gen.name; i++)
        guc_vars[num_vars++] = &ConfigureNamesBool[i].gen;

    for (i = 0; ConfigureNamesInt[i].gen.name; i++)
        guc_vars[num_vars++] = &ConfigureNamesInt[i].gen;

    for (i = 0; ConfigureNamesUInt[i].gen.name; i++)
        guc_vars[num_vars++] = &ConfigureNamesUInt[i].gen;

    for (i = 0; ConfigureNamesReal[i].gen.name; i++)
        guc_vars[num_vars++] = &ConfigureNamesReal[i].gen;

    for (i = 0; ConfigureNamesString[i].gen.name; i++)
        guc_vars[num_vars++] = &ConfigureNamesString[i].gen;

    for (i = 0; ConfigureNamesEnum[i].gen.name; i++)
        guc_vars[num_vars++] = &ConfigureNamesEnum[i].gen;

    if (guc_variables)
        free(guc_variables);
    guc_variables = guc_vars;
    num_guc_variables = num_vars;
    size_guc_variables = size_vars;
    qsort((void *) guc_variables, num_guc_variables,
          sizeof(struct config_generic *), guc_var_compare);
}

/*
 * Add a new GUC variable to the list of known variables. The
 * list is expanded if needed.
 */
static bool
add_guc_variable(struct config_generic *var, int elevel)
{
    if (num_guc_variables + 1 >= size_guc_variables)
    {
        /*
         * Increase the vector by 25%
         */
        int            size_vars = size_guc_variables + size_guc_variables / 4;
        struct config_generic **guc_vars;

        if (size_vars == 0)
        {
            size_vars = 100;
            guc_vars = (struct config_generic **)
                guc_malloc(elevel, size_vars * sizeof(struct config_generic *));
        }
        else
        {
            guc_vars = (struct config_generic **)
                guc_realloc(elevel, guc_variables, size_vars * sizeof(struct config_generic *));
        }

        if (guc_vars == NULL)
            return false;        /* out of memory */

        guc_variables = guc_vars;
        size_guc_variables = size_vars;
    }
    guc_variables[num_guc_variables++] = var;
    qsort((void *) guc_variables, num_guc_variables,
          sizeof(struct config_generic *), guc_var_compare);
    return true;
}

/*
 * Create and add a placeholder variable for a custom variable name.
 */
static struct config_generic *
add_placeholder_variable(const char *name, int elevel)
{
    size_t        sz = sizeof(struct config_string) + sizeof(char *);
    struct config_string *var;
    struct config_generic *gen;

    var = (struct config_string *) guc_malloc(elevel, sz);
    if (var == NULL)
        return NULL;
    memset(var, 0, sz);
    gen = &var->gen;

    gen->name = guc_strdup(elevel, name);
    if (gen->name == NULL)
    {
        free(var);
        return NULL;
    }

    gen->context = PGC_USERSET;
    gen->group = CUSTOM_OPTIONS;
    gen->short_desc = "GUC placeholder variable";
    gen->flags = GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | GUC_CUSTOM_PLACEHOLDER;
    gen->vartype = PGC_STRING;

    /*
     * The char* is allocated at the end of the struct since we have no
     * 'static' place to point to.  Note that the current value, as well as
     * the boot and reset values, start out NULL.
     */
    var->variable = (char **) (var + 1);

    if (!add_guc_variable((struct config_generic *) var, elevel))
    {
        free((void *) gen->name);
        free(var);
        return NULL;
    }

    return gen;
}

/*
 * Look up option NAME.  If it exists, return a pointer to its record,
 * else return NULL.  If create_placeholders is TRUE, we'll create a
 * placeholder record for a valid-looking custom variable name.
 */
static struct config_generic *
find_option(const char *name, bool create_placeholders, int elevel)
{
    const char **key = &name;
    struct config_generic **res;
    int            i;

    Assert(name);

    /*
     * By equating const char ** with struct config_generic *, we are assuming
     * the name field is first in config_generic.
     */
    res = (struct config_generic **) bsearch((void *) &key,
                                             (void *) guc_variables,
                                             num_guc_variables,
                                             sizeof(struct config_generic *),
                                             guc_var_compare);
    if (res)
        return *res;

    /*
     * See if the name is an obsolete name for a variable.  We assume that the
     * set of supported old names is short enough that a brute-force search is
     * the best way.
     */
    for (i = 0; map_old_guc_names[i] != NULL; i += 2)
    {
        if (guc_name_compare(name, map_old_guc_names[i]) == 0)
            return find_option(map_old_guc_names[i + 1], false, elevel);
    }

    if (create_placeholders)
    {
        /*
         * Check if the name is qualified, and if so, add a placeholder.
         */
        if (strchr(name, GUC_QUALIFIER_SEPARATOR) != NULL)
            return add_placeholder_variable(name, elevel);
    }

    /* Unknown name */
    return NULL;
}


/*
 * comparator for qsorting and bsearching guc_variables array
 */
static int
guc_var_compare(const void *a, const void *b)
{
    const struct config_generic *confa = *(struct config_generic *const *) a;
    const struct config_generic *confb = *(struct config_generic *const *) b;

    return guc_name_compare(confa->name, confb->name);
}

/*
 * the bare comparison function for GUC names
 */
static int
guc_name_compare(const char *namea, const char *nameb)
{// #lizard forgives
    /*
     * The temptation to use strcasecmp() here must be resisted, because the
     * array ordering has to remain stable across setlocale() calls. So, build
     * our own with a simple ASCII-only downcasing.
     */
    while (*namea && *nameb)
    {
        char        cha = *namea++;
        char        chb = *nameb++;

        if (cha >= 'A' && cha <= 'Z')
            cha += 'a' - 'A';
        if (chb >= 'A' && chb <= 'Z')
            chb += 'a' - 'A';
        if (cha != chb)
            return cha - chb;
    }
    if (*namea)
        return 1;                /* a is longer */
    if (*nameb)
        return -1;                /* b is longer */
    return 0;
}


/*
 * Initialize GUC options during program startup.
 *
 * Note that we cannot read the config file yet, since we have not yet
 * processed command-line switches.
 */
void
InitializeGUCOptions(void)
{
    int            i;

    /*
     * Before log_line_prefix could possibly receive a nonempty setting, make
     * sure that timezone processing is minimally alive (see elog.c).
     */
    pg_timezone_initialize();

    /*
     * Build sorted array of all GUC variables.
     */
    build_guc_variables();

    /*
     * Load all variables with their compiled-in defaults, and initialize
     * status fields as needed.
     */
    for (i = 0; i < num_guc_variables; i++)
    {
        InitializeOneGUCOption(guc_variables[i]);
    }

    guc_dirty = false;

    reporting_enabled = false;

    /*
     * Prevent any attempt to override the transaction modes from
     * non-interactive sources.
     */
    SetConfigOption("transaction_isolation", "default",
                    PGC_POSTMASTER, PGC_S_OVERRIDE);
    SetConfigOption("transaction_read_only", "no",
                    PGC_POSTMASTER, PGC_S_OVERRIDE);
    SetConfigOption("transaction_deferrable", "no",
                    PGC_POSTMASTER, PGC_S_OVERRIDE);

    /*
     * For historical reasons, some GUC parameters can receive defaults from
     * environment variables.  Process those settings.
     */
    InitializeGUCOptionsFromEnvironment();
}

/*
 * Assign any GUC values that can come from the server's environment.
 *
 * This is called from InitializeGUCOptions, and also from ProcessConfigFile
 * to deal with the possibility that a setting has been removed from
 * postgresql.conf and should now get a value from the environment.
 * (The latter is a kludge that should probably go away someday; if so,
 * fold this back into InitializeGUCOptions.)
 */
static void
InitializeGUCOptionsFromEnvironment(void)
{
    char       *env;
    long        stack_rlimit;

    env = getenv("PGPORT");
    if (env != NULL)
        SetConfigOption("port", env, PGC_POSTMASTER, PGC_S_ENV_VAR);

    env = getenv("PGDATESTYLE");
    if (env != NULL)
        SetConfigOption("datestyle", env, PGC_POSTMASTER, PGC_S_ENV_VAR);

    env = getenv("PGCLIENTENCODING");
    if (env != NULL)
        SetConfigOption("client_encoding", env, PGC_POSTMASTER, PGC_S_ENV_VAR);

    /*
     * rlimit isn't exactly an "environment variable", but it behaves about
     * the same.  If we can identify the platform stack depth rlimit, increase
     * default stack depth setting up to whatever is safe (but at most 2MB).
     */
    stack_rlimit = get_stack_depth_rlimit();
    if (stack_rlimit > 0)
    {
        long        new_limit = (stack_rlimit - STACK_DEPTH_SLOP) / 1024L;

        if (new_limit > 100)
        {
            char        limbuf[16];

            new_limit = Min(new_limit, 2048);
            sprintf(limbuf, "%ld", new_limit);
            SetConfigOption("max_stack_depth", limbuf,
                            PGC_POSTMASTER, PGC_S_ENV_VAR);
        }
    }
}

/*
 * Initialize one GUC option variable to its compiled-in default.
 *
 * Note: the reason for calling check_hooks is not that we think the boot_val
 * might fail, but that the hooks might wish to compute an "extra" struct.
 */
static void
InitializeOneGUCOption(struct config_generic *gconf)
{// #lizard forgives
    gconf->status = 0;
    gconf->source = PGC_S_DEFAULT;
    gconf->reset_source = PGC_S_DEFAULT;
    gconf->scontext = PGC_INTERNAL;
    gconf->reset_scontext = PGC_INTERNAL;
    gconf->stack = NULL;
    gconf->extra = NULL;
    gconf->sourcefile = NULL;
    gconf->sourceline = 0;

    switch (gconf->vartype)
    {
        case PGC_BOOL:
            {
                struct config_bool *conf = (struct config_bool *) gconf;
                bool        newval = conf->boot_val;
                void       *extra = NULL;

                if (!call_bool_check_hook(conf, &newval, &extra,
                                          PGC_S_DEFAULT, LOG))
                    elog(FATAL, "failed to initialize %s to %d",
                         conf->gen.name, (int) newval);
                if (conf->assign_hook)
                    (*conf->assign_hook) (newval, extra);
                *conf->variable = conf->reset_val = newval;
                conf->gen.extra = conf->reset_extra = extra;
                break;
            }
        case PGC_INT:
            {
                struct config_int *conf = (struct config_int *) gconf;
                int            newval = conf->boot_val;
                void       *extra = NULL;

                Assert(newval >= conf->min);
                Assert(newval <= conf->max);
                if (!call_int_check_hook(conf, &newval, &extra,
                                         PGC_S_DEFAULT, LOG))
                    elog(FATAL, "failed to initialize %s to %d",
                         conf->gen.name, newval);
                if (conf->assign_hook)
                    (*conf->assign_hook) (newval, extra);
                *conf->variable = conf->reset_val = newval;
                conf->gen.extra = conf->reset_extra = extra;
                break;
            }
        case PGC_UINT:
            {
                struct config_uint *conf = (struct config_uint *) gconf;
                uint        newval = conf->boot_val;
                void       *extra = NULL;

                Assert(newval >= conf->min);
                Assert(newval <= conf->max);
                if (!call_uint_check_hook(conf, &newval, &extra,
                                         PGC_S_DEFAULT, LOG))
                    elog(FATAL, "failed to initialize %s to %d",
                         conf->gen.name, newval);
                if (conf->assign_hook)
                    (*conf->assign_hook) (newval, extra);
                *conf->variable = conf->reset_val = newval;
                conf->gen.extra = conf->reset_extra = extra;
                break;
            }
        case PGC_REAL:
            {
                struct config_real *conf = (struct config_real *) gconf;
                double        newval = conf->boot_val;
                void       *extra = NULL;

                Assert(newval >= conf->min);
                Assert(newval <= conf->max);
                if (!call_real_check_hook(conf, &newval, &extra,
                                          PGC_S_DEFAULT, LOG))
                    elog(FATAL, "failed to initialize %s to %g",
                         conf->gen.name, newval);
                if (conf->assign_hook)
                    (*conf->assign_hook) (newval, extra);
                *conf->variable = conf->reset_val = newval;
                conf->gen.extra = conf->reset_extra = extra;
                break;
            }
        case PGC_STRING:
            {
                struct config_string *conf = (struct config_string *) gconf;
                char       *newval;
                void       *extra = NULL;

                /* non-NULL boot_val must always get strdup'd */
                if (conf->boot_val != NULL)
                    newval = guc_strdup(FATAL, conf->boot_val);
                else
                    newval = NULL;

                if (!call_string_check_hook(conf, &newval, &extra,
                                            PGC_S_DEFAULT, LOG))
                    elog(FATAL, "failed to initialize %s to \"%s\"",
                         conf->gen.name, newval ? newval : "");
                if (conf->assign_hook)
                    (*conf->assign_hook) (newval, extra);
                *conf->variable = conf->reset_val = newval;
                conf->gen.extra = conf->reset_extra = extra;
                break;
            }
        case PGC_ENUM:
            {
                struct config_enum *conf = (struct config_enum *) gconf;
                int            newval = conf->boot_val;
                void       *extra = NULL;

                if (!call_enum_check_hook(conf, &newval, &extra,
                                          PGC_S_DEFAULT, LOG))
                    elog(FATAL, "failed to initialize %s to %d",
                         conf->gen.name, newval);
                if (conf->assign_hook)
                    (*conf->assign_hook) (newval, extra);
                *conf->variable = conf->reset_val = newval;
                conf->gen.extra = conf->reset_extra = extra;
                break;
            }
    }
}


/*
 * Select the configuration files and data directory to be used, and
 * do the initial read of postgresql.conf.
 *
 * This is called after processing command-line switches.
 *        userDoption is the -D switch value if any (NULL if unspecified).
 *        progname is just for use in error messages.
 *
 * Returns true on success; on failure, prints a suitable error message
 * to stderr and returns false.
 */
bool
SelectConfigFiles(const char *userDoption, const char *progname)
{// #lizard forgives
    char       *configdir;
    char       *fname;
    struct stat stat_buf;

    /* configdir is -D option, or $PGDATA if no -D */
    if (userDoption)
        configdir = make_absolute_path(userDoption);
    else
        configdir = make_absolute_path(getenv("PGDATA"));

    if (configdir && stat(configdir, &stat_buf) != 0)
    {
        write_stderr("%s: could not access directory \"%s\": %s\n",
                     progname,
                     configdir,
                     strerror(errno));
        if (errno == ENOENT)
            write_stderr("Run initdb or pg_basebackup to initialize a PostgreSQL data directory.\n");
        return false;
    }

    /*
     * Find the configuration file: if config_file was specified on the
     * command line, use it, else use configdir/postgresql.conf.  In any case
     * ensure the result is an absolute path, so that it will be interpreted
     * the same way by future backends.
     */
    if (ConfigFileName)
        fname = make_absolute_path(ConfigFileName);
    else if (configdir)
    {
        fname = guc_malloc(FATAL,
                           strlen(configdir) + strlen(CONFIG_FILENAME) + 2);
        sprintf(fname, "%s/%s", configdir, CONFIG_FILENAME);
    }
    else
    {
        write_stderr("%s does not know where to find the server configuration file.\n"
                     "You must specify the --config-file or -D invocation "
                     "option or set the PGDATA environment variable.\n",
                     progname);
        return false;
    }

    /*
     * Set the ConfigFileName GUC variable to its final value, ensuring that
     * it can't be overridden later.
     */
    SetConfigOption("config_file", fname, PGC_POSTMASTER, PGC_S_OVERRIDE);
    free(fname);

    /*
     * Now read the config file for the first time.
     */
    if (stat(ConfigFileName, &stat_buf) != 0)
    {
        write_stderr("%s: could not access the server configuration file \"%s\": %s\n",
                     progname, ConfigFileName, strerror(errno));
        free(configdir);
        return false;
    }

    /*
     * Read the configuration file for the first time.  This time only the
     * data_directory parameter is picked up to determine the data directory,
     * so that we can read the PG_AUTOCONF_FILENAME file next time.
     */
    ProcessConfigFile(PGC_POSTMASTER);

    /*
     * If the data_directory GUC variable has been set, use that as DataDir;
     * otherwise use configdir if set; else punt.
     *
     * Note: SetDataDir will copy and absolute-ize its argument, so we don't
     * have to.
     */
    if (data_directory)
        SetDataDir(data_directory);
    else if (configdir)
        SetDataDir(configdir);
    else
    {
        write_stderr("%s does not know where to find the database system data.\n"
                     "This can be specified as \"data_directory\" in \"%s\", "
                     "or by the -D invocation option, or by the "
                     "PGDATA environment variable.\n",
                     progname, ConfigFileName);
        return false;
    }

    /*
     * Reflect the final DataDir value back into the data_directory GUC var.
     * (If you are wondering why we don't just make them a single variable,
     * it's because the EXEC_BACKEND case needs DataDir to be transmitted to
     * child backends specially.  XXX is that still true?  Given that we now
     * chdir to DataDir, EXEC_BACKEND can read the config file without knowing
     * DataDir in advance.)
     */
    SetConfigOption("data_directory", DataDir, PGC_POSTMASTER, PGC_S_OVERRIDE);

    /*
     * Now read the config file a second time, allowing any settings in the
     * PG_AUTOCONF_FILENAME file to take effect.  (This is pretty ugly, but
     * since we have to determine the DataDir before we can find the autoconf
     * file, the alternatives seem worse.)
     */
    ProcessConfigFile(PGC_POSTMASTER);

    /*
     * If timezone_abbreviations wasn't set in the configuration file, install
     * the default value.  We do it this way because we can't safely install a
     * "real" value until my_exec_path is set, which may not have happened
     * when InitializeGUCOptions runs, so the bootstrap default value cannot
     * be the real desired default.
     */
    pg_timezone_abbrev_initialize();

    /*
     * Figure out where pg_hba.conf is, and make sure the path is absolute.
     */
    if (HbaFileName)
        fname = make_absolute_path(HbaFileName);
    else if (configdir)
    {
        fname = guc_malloc(FATAL,
                           strlen(configdir) + strlen(HBA_FILENAME) + 2);
        sprintf(fname, "%s/%s", configdir, HBA_FILENAME);
    }
    else
    {
        write_stderr("%s does not know where to find the \"hba\" configuration file.\n"
                     "This can be specified as \"hba_file\" in \"%s\", "
                     "or by the -D invocation option, or by the "
                     "PGDATA environment variable.\n",
                     progname, ConfigFileName);
        return false;
    }
    SetConfigOption("hba_file", fname, PGC_POSTMASTER, PGC_S_OVERRIDE);
    free(fname);

    /*
     * Likewise for pg_ident.conf.
     */
    if (IdentFileName)
        fname = make_absolute_path(IdentFileName);
    else if (configdir)
    {
        fname = guc_malloc(FATAL,
                           strlen(configdir) + strlen(IDENT_FILENAME) + 2);
        sprintf(fname, "%s/%s", configdir, IDENT_FILENAME);
    }
    else
    {
        write_stderr("%s does not know where to find the \"ident\" configuration file.\n"
                     "This can be specified as \"ident_file\" in \"%s\", "
                     "or by the -D invocation option, or by the "
                     "PGDATA environment variable.\n",
                     progname, ConfigFileName);
        return false;
    }
    SetConfigOption("ident_file", fname, PGC_POSTMASTER, PGC_S_OVERRIDE);
    free(fname);

    free(configdir);

    return true;
}


/*
 * Reset all options to their saved default values (implements RESET ALL)
 */
void
ResetAllOptions(void)
{// #lizard forgives
    int            i;

    for (i = 0; i < num_guc_variables; i++)
    {
        struct config_generic *gconf = guc_variables[i];

        /* Don't reset non-SET-able values */
        if (gconf->context != PGC_SUSET &&
            gconf->context != PGC_USERSET)
            continue;
        /* Don't reset if special exclusion from RESET ALL */
        if (gconf->flags & GUC_NO_RESET_ALL)
            continue;
        /* No need to reset if wasn't SET */
        if (gconf->source <= PGC_S_OVERRIDE)
            continue;

        /* Save old value to support transaction abort */
        push_old_value(gconf, GUC_ACTION_SET);

        switch (gconf->vartype)
        {
            case PGC_BOOL:
                {
                    struct config_bool *conf = (struct config_bool *) gconf;

                    if (conf->assign_hook)
                        (*conf->assign_hook) (conf->reset_val,
                                              conf->reset_extra);
                    *conf->variable = conf->reset_val;
                    set_extra_field(&conf->gen, &conf->gen.extra,
                                    conf->reset_extra);
                    break;
                }
            case PGC_INT:
                {
                    struct config_int *conf = (struct config_int *) gconf;

                    if (conf->assign_hook)
                        (*conf->assign_hook) (conf->reset_val,
                                              conf->reset_extra);
                    *conf->variable = conf->reset_val;
                    set_extra_field(&conf->gen, &conf->gen.extra,
                                    conf->reset_extra);
                    break;
                }
            case PGC_UINT:
                {
                    struct config_uint *conf = (struct config_uint *) gconf;

                    if (conf->assign_hook)
                        (*conf->assign_hook) (conf->reset_val,
                                              conf->reset_extra);
                    *conf->variable = conf->reset_val;
                    set_extra_field(&conf->gen, &conf->gen.extra,
                                    conf->reset_extra);
                    break;
                }
            case PGC_REAL:
                {
                    struct config_real *conf = (struct config_real *) gconf;

                    if (conf->assign_hook)
                        (*conf->assign_hook) (conf->reset_val,
                                              conf->reset_extra);
                    *conf->variable = conf->reset_val;
                    set_extra_field(&conf->gen, &conf->gen.extra,
                                    conf->reset_extra);
                    break;
                }
            case PGC_STRING:
                {
                    struct config_string *conf = (struct config_string *) gconf;

                    if (conf->assign_hook)
                        (*conf->assign_hook) (conf->reset_val,
                                              conf->reset_extra);
                    set_string_field(conf, conf->variable, conf->reset_val);
                    set_extra_field(&conf->gen, &conf->gen.extra,
                                    conf->reset_extra);
                    break;
                }
            case PGC_ENUM:
                {
                    struct config_enum *conf = (struct config_enum *) gconf;

                    if (conf->assign_hook)
                        (*conf->assign_hook) (conf->reset_val,
                                              conf->reset_extra);
                    *conf->variable = conf->reset_val;
                    set_extra_field(&conf->gen, &conf->gen.extra,
                                    conf->reset_extra);
                    break;
                }
        }

        gconf->source = gconf->reset_source;
        gconf->scontext = gconf->reset_scontext;

        if (gconf->flags & GUC_REPORT)
            ReportGUCOption(gconf);
    }
}


/*
 * push_old_value
 *        Push previous state during transactional assignment to a GUC variable.
 */
static void
push_old_value(struct config_generic *gconf, GucAction action)
{// #lizard forgives
    GucStack   *stack;

    /* If we're not inside a nest level, do nothing */
    if (GUCNestLevel == 0)
        return;

    /* Do we already have a stack entry of the current nest level? */
    stack = gconf->stack;
    if (stack && stack->nest_level >= GUCNestLevel)
    {
        /* Yes, so adjust its state if necessary */
        Assert(stack->nest_level == GUCNestLevel);
        switch (action)
        {
            case GUC_ACTION_SET:
                /* SET overrides any prior action at same nest level */
                if (stack->state == GUC_SET_LOCAL)
                {
                    /* must discard old masked value */
                    discard_stack_value(gconf, &stack->masked);
                }
                stack->state = GUC_SET;
                break;
            case GUC_ACTION_LOCAL:
                if (stack->state == GUC_SET)
                {
                    /* SET followed by SET LOCAL, remember SET's value */
                    stack->masked_scontext = gconf->scontext;
                    set_stack_value(gconf, &stack->masked);
                    stack->state = GUC_SET_LOCAL;
                }
                /* in all other cases, no change to stack entry */
                break;
            case GUC_ACTION_SAVE:
                /* Could only have a prior SAVE of same variable */
                Assert(stack->state == GUC_SAVE);
                break;
        }
        Assert(guc_dirty);        /* must be set already */
        return;
    }

    /*
     * Push a new stack entry
     *
     * We keep all the stack entries in TopTransactionContext for simplicity.
     */
    stack = (GucStack *) MemoryContextAllocZero(TopTransactionContext,
                                                sizeof(GucStack));

    stack->prev = gconf->stack;
    stack->nest_level = GUCNestLevel;
    switch (action)
    {
        case GUC_ACTION_SET:
            stack->state = GUC_SET;
            break;
        case GUC_ACTION_LOCAL:
            stack->state = GUC_LOCAL;
            break;
        case GUC_ACTION_SAVE:
            stack->state = GUC_SAVE;
            break;
    }
    stack->source = gconf->source;
    stack->scontext = gconf->scontext;
    set_stack_value(gconf, &stack->prior);

    gconf->stack = stack;

    /* Ensure we remember to pop at end of xact */
    guc_dirty = true;
}


/*
 * Do GUC processing at main transaction start.
 */
void
AtStart_GUC(void)
{
    /*
     * The nest level should be 0 between transactions; if it isn't, somebody
     * didn't call AtEOXact_GUC, or called it with the wrong nestLevel.  We
     * throw a warning but make no other effort to clean up.
     */
    if (GUCNestLevel != 0)
        elog(WARNING, "GUC nest level = %d at transaction start",
             GUCNestLevel);
    GUCNestLevel = 1;
}

/*
 * Enter a new nesting level for GUC values.  This is called at subtransaction
 * start, and when entering a function that has proconfig settings, and in
 * some other places where we want to set GUC variables transiently.
 * NOTE we must not risk error here, else subtransaction start will be unhappy.
 */
int
NewGUCNestLevel(void)
{
    return ++GUCNestLevel;
}

/*
 * Do GUC processing at transaction or subtransaction commit or abort, or
 * when exiting a function that has proconfig settings, or when undoing a
 * transient assignment to some GUC variables.  (The name is thus a bit of
 * a misnomer; perhaps it should be ExitGUCNestLevel or some such.)
 * During abort, we discard all GUC settings that were applied at nesting
 * levels >= nestLevel.  nestLevel == 1 corresponds to the main transaction.
 */
void
AtEOXact_GUC(bool isCommit, int nestLevel)
{// #lizard forgives
    bool        still_dirty;
    int            i;

    /*
     * Note: it's possible to get here with GUCNestLevel == nestLevel-1 during
     * abort, if there is a failure during transaction start before
     * AtStart_GUC is called.
     */
    Assert(nestLevel > 0 &&
           (nestLevel <= GUCNestLevel ||
            (nestLevel == GUCNestLevel + 1 && !isCommit)));

    /* Quick exit if nothing's changed in this transaction */
    if (!guc_dirty)
    {
        GUCNestLevel = nestLevel - 1;
        return;
    }

    still_dirty = false;
    for (i = 0; i < num_guc_variables; i++)
    {
        struct config_generic *gconf = guc_variables[i];
        GucStack   *stack;

        /*
         * Process and pop each stack entry within the nest level. To simplify
         * fmgr_security_definer() and other places that use GUC_ACTION_SAVE,
         * we allow failure exit from code that uses a local nest level to be
         * recovered at the surrounding transaction or subtransaction abort;
         * so there could be more than one stack entry to pop.
         */
        while ((stack = gconf->stack) != NULL &&
               stack->nest_level >= nestLevel)
        {
            GucStack   *prev = stack->prev;
            bool        restorePrior = false;
            bool        restoreMasked = false;
            bool        changed;

            /*
             * In this next bit, if we don't set either restorePrior or
             * restoreMasked, we must "discard" any unwanted fields of the
             * stack entries to avoid leaking memory.  If we do set one of
             * those flags, unused fields will be cleaned up after restoring.
             */
            if (!isCommit)        /* if abort, always restore prior value */
                restorePrior = true;
            else if (stack->state == GUC_SAVE)
                restorePrior = true;
            else if (stack->nest_level == 1)
            {
                /* transaction commit */
                if (stack->state == GUC_SET_LOCAL)
                    restoreMasked = true;
                else if (stack->state == GUC_SET)
                {
                    /* we keep the current active value */
                    discard_stack_value(gconf, &stack->prior);
                }
                else            /* must be GUC_LOCAL */
                    restorePrior = true;
            }
            else if (prev == NULL ||
                     prev->nest_level < stack->nest_level - 1)
            {
                /* decrement entry's level and do not pop it */
                stack->nest_level--;
                continue;
            }
            else
            {
                /*
                 * We have to merge this stack entry into prev. See README for
                 * discussion of this bit.
                 */
                switch (stack->state)
                {
                    case GUC_SAVE:
                        Assert(false);    /* can't get here */

                    case GUC_SET:
                        /* next level always becomes SET */
                        discard_stack_value(gconf, &stack->prior);
                        if (prev->state == GUC_SET_LOCAL)
                            discard_stack_value(gconf, &prev->masked);
                        prev->state = GUC_SET;
                        break;

                    case GUC_LOCAL:
                        if (prev->state == GUC_SET)
                        {
                            /* LOCAL migrates down */
                            prev->masked_scontext = stack->scontext;
                            prev->masked = stack->prior;
                            prev->state = GUC_SET_LOCAL;
                        }
                        else
                        {
                            /* else just forget this stack level */
                            discard_stack_value(gconf, &stack->prior);
                        }
                        break;

                    case GUC_SET_LOCAL:
                        /* prior state at this level no longer wanted */
                        discard_stack_value(gconf, &stack->prior);
                        /* copy down the masked state */
                        prev->masked_scontext = stack->masked_scontext;
                        if (prev->state == GUC_SET_LOCAL)
                            discard_stack_value(gconf, &prev->masked);
                        prev->masked = stack->masked;
                        prev->state = GUC_SET_LOCAL;
                        break;
                }
            }

            changed = false;

            if (restorePrior || restoreMasked)
            {
                /* Perform appropriate restoration of the stacked value */
                config_var_value newvalue;
                GucSource    newsource;
                GucContext    newscontext;

                if (restoreMasked)
                {
                    newvalue = stack->masked;
                    newsource = PGC_S_SESSION;
                    newscontext = stack->masked_scontext;
                }
                else
                {
                    newvalue = stack->prior;
                    newsource = stack->source;
                    newscontext = stack->scontext;
                }

                switch (gconf->vartype)
                {
                    case PGC_BOOL:
                        {
                            struct config_bool *conf = (struct config_bool *) gconf;
                            bool        newval = newvalue.val.boolval;
                            void       *newextra = newvalue.extra;

                            if (*conf->variable != newval ||
                                conf->gen.extra != newextra)
                            {
                                if (conf->assign_hook)
                                    (*conf->assign_hook) (newval, newextra);
                                *conf->variable = newval;
                                set_extra_field(&conf->gen, &conf->gen.extra,
                                                newextra);
                                changed = true;
                            }
                            break;
                        }
                    case PGC_INT:
                        {
                            struct config_int *conf = (struct config_int *) gconf;
                            int            newval = newvalue.val.intval;
                            void       *newextra = newvalue.extra;

                            if (*conf->variable != newval ||
                                conf->gen.extra != newextra)
                            {
                                if (conf->assign_hook)
                                    (*conf->assign_hook) (newval, newextra);
                                *conf->variable = newval;
                                set_extra_field(&conf->gen, &conf->gen.extra,
                                                newextra);
                                changed = true;
                            }
                            break;
                        }
                    case PGC_UINT:
                        {
                            struct config_uint *conf = (struct config_uint *) gconf;
                            uint        newval = newvalue.val.intval;
                            void       *newextra = newvalue.extra;

                            if (*conf->variable != newval ||
                                conf->gen.extra != newextra)
                            {
                                if (conf->assign_hook)
                                    (*conf->assign_hook) (newval, newextra);
                                *conf->variable = newval;
                                set_extra_field(&conf->gen, &conf->gen.extra,
                                                newextra);
                                changed = true;
                            }
                            break;
                        }
                    case PGC_REAL:
                        {
                            struct config_real *conf = (struct config_real *) gconf;
                            double        newval = newvalue.val.realval;
                            void       *newextra = newvalue.extra;

                            if (*conf->variable != newval ||
                                conf->gen.extra != newextra)
                            {
                                if (conf->assign_hook)
                                    (*conf->assign_hook) (newval, newextra);
                                *conf->variable = newval;
                                set_extra_field(&conf->gen, &conf->gen.extra,
                                                newextra);
                                changed = true;
                            }
                            break;
                        }
                    case PGC_STRING:
                        {
                            struct config_string *conf = (struct config_string *) gconf;
                            char       *newval = newvalue.val.stringval;
                            void       *newextra = newvalue.extra;

                            if (*conf->variable != newval ||
                                conf->gen.extra != newextra)
                            {
                                if (conf->assign_hook)
                                    (*conf->assign_hook) (newval, newextra);
                                set_string_field(conf, conf->variable, newval);
                                set_extra_field(&conf->gen, &conf->gen.extra,
                                                newextra);
                                changed = true;
                            }

                            /*
                             * Release stacked values if not used anymore. We
                             * could use discard_stack_value() here, but since
                             * we have type-specific code anyway, might as
                             * well inline it.
                             */
                            set_string_field(conf, &stack->prior.val.stringval, NULL);
                            set_string_field(conf, &stack->masked.val.stringval, NULL);
                            break;
                        }
                    case PGC_ENUM:
                        {
                            struct config_enum *conf = (struct config_enum *) gconf;
                            int            newval = newvalue.val.enumval;
                            void       *newextra = newvalue.extra;

                            if (*conf->variable != newval ||
                                conf->gen.extra != newextra)
                            {
                                if (conf->assign_hook)
                                    (*conf->assign_hook) (newval, newextra);
                                *conf->variable = newval;
                                set_extra_field(&conf->gen, &conf->gen.extra,
                                                newextra);
                                changed = true;
                            }
                            break;
                        }
                }

                /*
                 * Release stacked extra values if not used anymore.
                 */
                set_extra_field(gconf, &(stack->prior.extra), NULL);
                set_extra_field(gconf, &(stack->masked.extra), NULL);

                /* And restore source information */
                gconf->source = newsource;
                gconf->scontext = newscontext;
            }

            if (changed)
            {
                const char        *newvalStr = NULL;

                /* XXX perhaps this should use is_missing=false, not sure */
                newvalStr = GetConfigOptionByName(gconf->name, NULL, true);

                if (newvalStr)
                    PGXCNodeSetParam((stack->state == GUC_LOCAL), gconf->name,
                            newvalStr, gconf->flags);
            }

            /* Finish popping the state stack */
            gconf->stack = prev;
            pfree(stack);

            /* Report new value if we changed it */
            if (changed && (gconf->flags & GUC_REPORT))
                ReportGUCOption(gconf);
        }                        /* end of stack-popping loop */

        if (stack != NULL)
            still_dirty = true;
    }

    /* If there are no remaining stack entries, we can reset guc_dirty */
    guc_dirty = still_dirty;

    /* Update nesting level */
    GUCNestLevel = nestLevel - 1;
}


/*
 * Start up automatic reporting of changes to variables marked GUC_REPORT.
 * This is executed at completion of backend startup.
 */
void
BeginReportingGUCOptions(void)
{
    int            i;

    /*
     * Don't do anything unless talking to an interactive frontend of protocol
     * 3.0 or later.
     */
    if (whereToSendOutput != DestRemote ||
        PG_PROTOCOL_MAJOR(FrontendProtocol) < 3)
        return;

    reporting_enabled = true;

    /* Transmit initial values of interesting variables */
    for (i = 0; i < num_guc_variables; i++)
    {
        struct config_generic *conf = guc_variables[i];

        if (conf->flags & GUC_REPORT)
            ReportGUCOption(conf);
    }
}

/*
 * ReportGUCOption: if appropriate, transmit option value to frontend
 */
static void
ReportGUCOption(struct config_generic *record)
{
    if (reporting_enabled && (record->flags & GUC_REPORT))
    {
        char       *val = _ShowOption(record, false);
        StringInfoData msgbuf;

        pq_beginmessage(&msgbuf, 'S');
        pq_sendstring(&msgbuf, record->name);
        pq_sendstring(&msgbuf, val);
        pq_endmessage(&msgbuf);

        pfree(val);
    }
}

/*
 * Convert a value from one of the human-friendly units ("kB", "min" etc.)
 * to the given base unit.  'value' and 'unit' are the input value and unit
 * to convert from.  The converted value is stored in *base_value.
 *
 * Returns true on success, false if the input unit is not recognized.
 */
static bool
convert_to_base_unit(int64 value, const char *unit,
                     int base_unit, int64 *base_value)
{
    const unit_conversion *table;
    int            i;

    if (base_unit & GUC_UNIT_MEMORY)
        table = memory_unit_conversion_table;
    else
        table = time_unit_conversion_table;

    for (i = 0; *table[i].unit; i++)
    {
        if (base_unit == table[i].base_unit &&
            strcmp(unit, table[i].unit) == 0)
        {
            if (table[i].multiplier < 0)
                *base_value = value / (-table[i].multiplier);
            else
                *base_value = value * table[i].multiplier;
            return true;
        }
    }
    return false;
}

/*
 * Convert a value from one of the human-friendly units ("kB", "min" etc.)
 * to the given base unit.  'value' and 'unit' are the input value and unit
 * to convert from.  The converted value is stored in *base_value.
 *
 * Returns true on success, false if the input unit is not recognized.
 */
static bool
convert_to_base_unit_unsigned(uint64 value, const char *unit,
                              int base_unit, uint64 *base_value)
{
    const unit_conversion *table;
    int            i;

    if (base_unit & GUC_UNIT_MEMORY)
        table = memory_unit_conversion_table;
    else
        table = time_unit_conversion_table;

    for (i = 0; *table[i].unit; i++)
    {
        if (base_unit == table[i].base_unit &&
            strcmp(unit, table[i].unit) == 0)
        {
            if (table[i].multiplier < 0)
                *base_value = value / (-table[i].multiplier);
            else
                *base_value = value * table[i].multiplier;
            return true;
        }
    }
    return false;
}

/*
 * Convert a value in some base unit to a human-friendly unit.  The output
 * unit is chosen so that it's the greatest unit that can represent the value
 * without loss.  For example, if the base unit is GUC_UNIT_KB, 1024 is
 * converted to 1 MB, but 1025 is represented as 1025 kB.
 */
static void
convert_from_base_unit(int64 base_value, int base_unit,
                       int64 *value, const char **unit)
{
    const unit_conversion *table;
    int            i;

    *unit = NULL;

    if (base_unit & GUC_UNIT_MEMORY)
        table = memory_unit_conversion_table;
    else
        table = time_unit_conversion_table;

    for (i = 0; *table[i].unit; i++)
    {
        if (base_unit == table[i].base_unit)
        {
            /*
             * Accept the first conversion that divides the value evenly. We
             * assume that the conversions for each base unit are ordered from
             * greatest unit to the smallest!
             */
            if (table[i].multiplier < 0)
            {
                *value = base_value * (-table[i].multiplier);
                *unit = table[i].unit;
                break;
            }
            else if (base_value % table[i].multiplier == 0)
            {
                *value = base_value / table[i].multiplier;
                *unit = table[i].unit;
                break;
            }
        }
    }

    Assert(*unit != NULL);
}


/*
 * Try to parse value as an integer.  The accepted formats are the
 * usual decimal, octal, or hexadecimal formats, optionally followed by
 * a unit name if "flags" indicates a unit is allowed.
 *
 * If the string parses okay, return true, else false.
 * If okay and result is not NULL, return the value in *result.
 * If not okay and hintmsg is not NULL, *hintmsg is set to a suitable
 *    HINT message, or NULL if no hint provided.
 */
bool
parse_int(const char *value, int *result, int flags, const char **hintmsg)
{// #lizard forgives
    int64        val;
    char       *endptr;

    /* To suppress compiler warnings, always set output params */
    if (result)
        *result = 0;
    if (hintmsg)
        *hintmsg = NULL;

    /* We assume here that int64 is at least as wide as long */
    errno = 0;
    val = strtol(value, &endptr, 0);

    if (endptr == value)
        return false;            /* no HINT for integer syntax error */

    if (errno == ERANGE || val != (int64) ((int32) val))
    {
        if (hintmsg)
            *hintmsg = gettext_noop("Value exceeds integer range.");
        return false;
    }

    /* allow whitespace between integer and unit */
    while (isspace((unsigned char) *endptr))
        endptr++;

    /* Handle possible unit */
    if (*endptr != '\0')
    {
        char        unit[MAX_UNIT_LEN + 1];
        int            unitlen;
        bool        converted = false;

        if ((flags & GUC_UNIT) == 0)
            return false;        /* this setting does not accept a unit */

        unitlen = 0;
        while (*endptr != '\0' && !isspace((unsigned char) *endptr) &&
               unitlen < MAX_UNIT_LEN)
            unit[unitlen++] = *(endptr++);
        unit[unitlen] = '\0';
        /* allow whitespace after unit */
        while (isspace((unsigned char) *endptr))
            endptr++;

        if (*endptr == '\0')
            converted = convert_to_base_unit(val, unit, (flags & GUC_UNIT),
                                             &val);
        if (!converted)
        {
            /* invalid unit, or garbage after the unit; set hint and fail. */
            if (hintmsg)
            {
                if (flags & GUC_UNIT_MEMORY)
                    *hintmsg = memory_units_hint;
                else
                    *hintmsg = time_units_hint;
            }
            return false;
        }

        /* Check for overflow due to units conversion */
        if (val != (int64) ((int32) val))
        {
            if (hintmsg)
                *hintmsg = gettext_noop("Value exceeds integer range.");
            return false;
        }
    }

    if (result)
        *result = (int) val;
    return true;
}


/*
 * Try to parse value as an unsigned integer.  The accepted formats are the
 * usual decimal, octal, or hexadecimal formats, optionally followed by
 * a unit name if "flags" indicates a unit is allowed.
 *
 * If the string parses okay, return true, else false.
 * If okay and result is not NULL, return the value in *result.
 * If not okay and hintmsg is not NULL, *hintmsg is set to a suitable
 *    HINT message, or NULL if no hint provided.
 */
bool
parse_uint(const char *value, uint *result, int flags, const char **hintmsg)
{// #lizard forgives
    uint64        val;
    char       *endptr;

    /* To suppress compiler warnings, always set output params */
    if (result)
        *result = 0;
    if (hintmsg)
        *hintmsg = NULL;

    /* We assume here that int64 is at least as wide as long */
    errno = 0;
    val = strtoul(value, &endptr, 0);

    if (endptr == value)
        return false;            /* no HINT for integer syntax error */

	if (errno == ERANGE || val != (uint64) ((uint32) val))
    {
        if (hintmsg)
            *hintmsg = gettext_noop("Value exceeds integer range.");
        return false;
    }

    /* allow whitespace between integer and unit */
    while (isspace((unsigned char) *endptr))
        endptr++;

    /* Handle possible unit */
    if (*endptr != '\0')
    {
        char        unit[MAX_UNIT_LEN + 1];
        int            unitlen;
        bool        converted = false;

        if ((flags & GUC_UNIT) == 0)
            return false;        /* this setting does not accept a unit */

        unitlen = 0;
        while (*endptr != '\0' && !isspace((unsigned char) *endptr) &&
               unitlen < MAX_UNIT_LEN)
            unit[unitlen++] = *(endptr++);
        unit[unitlen] = '\0';
        /* allow whitespace after unit */
        while (isspace((unsigned char) *endptr))
            endptr++;

        if (*endptr == '\0')
            converted = convert_to_base_unit_unsigned(val, unit,
                                                      (flags & GUC_UNIT),
                                                      &val);
        if (!converted)
        {
            /* invalid unit, or garbage after the unit; set hint and fail. */
            if (hintmsg)
            {
                if (flags & GUC_UNIT_MEMORY)
                    *hintmsg = memory_units_hint;
                else
                    *hintmsg = time_units_hint;
            }
            return false;
        }

        /* Check for overflow due to units conversion */
		if (val != (uint64) ((uint32) val))
        {
            if (hintmsg)
                *hintmsg = gettext_noop("Value exceeds integer range.");
            return false;
        }
    }

    if (result)
        *result = (int) val;
    return true;
}



/*
 * Try to parse value as a floating point number in the usual format.
 * If the string parses okay, return true, else false.
 * If okay and result is not NULL, return the value in *result.
 */
bool
parse_real(const char *value, double *result)
{
    double        val;
    char       *endptr;

    if (result)
        *result = 0;            /* suppress compiler warning */

    errno = 0;
    val = strtod(value, &endptr);
    if (endptr == value || errno == ERANGE)
        return false;

    /* allow whitespace after number */
    while (isspace((unsigned char) *endptr))
        endptr++;
    if (*endptr != '\0')
        return false;

    if (result)
        *result = val;
    return true;
}


/*
 * Lookup the name for an enum option with the selected value.
 * Should only ever be called with known-valid values, so throws
 * an elog(ERROR) if the enum option is not found.
 *
 * The returned string is a pointer to static data and not
 * allocated for modification.
 */
const char *
config_enum_lookup_by_value(struct config_enum *record, int val)
{
    const struct config_enum_entry *entry;

    for (entry = record->options; entry && entry->name; entry++)
    {
        if (entry->val == val)
            return entry->name;
    }

    elog(ERROR, "could not find enum option %d for %s",
         val, record->gen.name);
    return NULL;                /* silence compiler */
}


/*
 * Lookup the value for an enum option with the selected name
 * (case-insensitive).
 * If the enum option is found, sets the retval value and returns
 * true. If it's not found, return FALSE and retval is set to 0.
 */
bool
config_enum_lookup_by_name(struct config_enum *record, const char *value,
                           int *retval)
{
    const struct config_enum_entry *entry;

    for (entry = record->options; entry && entry->name; entry++)
    {
        if (pg_strcasecmp(value, entry->name) == 0)
        {
            *retval = entry->val;
            return TRUE;
        }
    }

    *retval = 0;
    return FALSE;
}


/*
 * Return a list of all available options for an enum, excluding
 * hidden ones, separated by the given separator.
 * If prefix is non-NULL, it is added before the first enum value.
 * If suffix is non-NULL, it is added to the end of the string.
 */
static char *
config_enum_get_options(struct config_enum *record, const char *prefix,
                        const char *suffix, const char *separator)
{
    const struct config_enum_entry *entry;
    StringInfoData retstr;
    int            seplen;

    initStringInfo(&retstr);
    appendStringInfoString(&retstr, prefix);

    seplen = strlen(separator);
    for (entry = record->options; entry && entry->name; entry++)
    {
        if (!entry->hidden)
        {
            appendStringInfoString(&retstr, entry->name);
            appendBinaryStringInfo(&retstr, separator, seplen);
        }
    }

    /*
     * All the entries may have been hidden, leaving the string empty if no
     * prefix was given. This indicates a broken GUC setup, since there is no
     * use for an enum without any values, so we just check to make sure we
     * don't write to invalid memory instead of actually trying to do
     * something smart with it.
     */
    if (retstr.len >= seplen)
    {
        /* Replace final separator */
        retstr.data[retstr.len - seplen] = '\0';
        retstr.len -= seplen;
    }

    appendStringInfoString(&retstr, suffix);

    return retstr.data;
}

/*
 * Parse and validate a proposed value for the specified configuration
 * parameter.
 *
 * This does built-in checks (such as range limits for an integer parameter)
 * and also calls any check hook the parameter may have.
 *
 * record: GUC variable's info record
 * name: variable name (should match the record of course)
 * value: proposed value, as a string
 * source: identifies source of value (check hooks may need this)
 * elevel: level to log any error reports at
 * newval: on success, converted parameter value is returned here
 * newextra: on success, receives any "extra" data returned by check hook
 *    (caller must initialize *newextra to NULL)
 *
 * Returns true if OK, false if not (or throws error, if elevel >= ERROR)
 */
static bool
parse_and_validate_value(struct config_generic *record,
                         const char *name, const char *value,
                         GucSource source, int elevel,
                         union config_var_val *newval, void **newextra)
{// #lizard forgives
    switch (record->vartype)
    {
        case PGC_BOOL:
            {
                struct config_bool *conf = (struct config_bool *) record;

                if (!parse_bool(value, &newval->boolval))
                {
                    ereport(elevel,
                            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                             errmsg("parameter \"%s\" requires a Boolean value",
                                    name)));
                    return false;
                }

                if (!call_bool_check_hook(conf, &newval->boolval, newextra,
                                          source, elevel))
                    return false;
            }
            break;
        case PGC_INT:
            {
                struct config_int *conf = (struct config_int *) record;
                const char *hintmsg;

                if (!parse_int(value, &newval->intval,
                               conf->gen.flags, &hintmsg))
                {
                    ereport(elevel,
                            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                             errmsg("invalid value for parameter \"%s\": \"%s\"",
                                    name, value),
                             hintmsg ? errhint("%s", _(hintmsg)) : 0));
                    return false;
                }

                if (newval->intval < conf->min || newval->intval > conf->max)
                {
                    ereport(elevel,
                            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                             errmsg("%d is outside the valid range for parameter \"%s\" (%d .. %d)",
                                    newval->intval, name,
                                    conf->min, conf->max)));
                    return false;
                }

                if (!call_int_check_hook(conf, &newval->intval, newextra,
                                         source, elevel))
                    return false;
            }
            break;
        case PGC_UINT:
            {
                struct config_uint *conf = (struct config_uint *) record;
                const char *hintmsg;

                if (!parse_uint(value, &newval->uintval,
                               conf->gen.flags, &hintmsg))
                {
                    ereport(elevel,
                            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                             errmsg("invalid value for parameter \"%s\": \"%s\"",
                                    name, value),
                             hintmsg ? errhint("%s", _(hintmsg)) : 0));
                    return false;
                }

				if (newval->uintval < conf->min || newval->uintval > conf->max)
                {
                    ereport(elevel,
                            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                             errmsg("%d is outside the valid range for parameter \"%s\" (%d .. %d)",
									newval->uintval, name,
                                    conf->min, conf->max)));
                    return false;
                }

                if (!call_uint_check_hook(conf, &newval->uintval, newextra,
                                         source, elevel))
                    return false;
            }
            break;
        case PGC_REAL:
            {
                struct config_real *conf = (struct config_real *) record;

                if (!parse_real(value, &newval->realval))
                {
                    ereport(elevel,
                            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                             errmsg("parameter \"%s\" requires a numeric value",
                                    name)));
                    return false;
                }

                if (newval->realval < conf->min || newval->realval > conf->max)
                {
                    ereport(elevel,
                            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                             errmsg("%g is outside the valid range for parameter \"%s\" (%g .. %g)",
                                    newval->realval, name,
                                    conf->min, conf->max)));
                    return false;
                }

                if (!call_real_check_hook(conf, &newval->realval, newextra,
                                          source, elevel))
                    return false;
            }
            break;
        case PGC_STRING:
            {
                struct config_string *conf = (struct config_string *) record;

                /*
                 * The value passed by the caller could be transient, so we
                 * always strdup it.
                 */
                newval->stringval = guc_strdup(elevel, value);
                if (newval->stringval == NULL)
                    return false;

                /*
                 * The only built-in "parsing" check we have is to apply
                 * truncation if GUC_IS_NAME.
                 */
                if (conf->gen.flags & GUC_IS_NAME)
                    truncate_identifier(newval->stringval,
                                        strlen(newval->stringval),
                                        true);

                if (!call_string_check_hook(conf, &newval->stringval, newextra,
                                            source, elevel))
                {
                    free(newval->stringval);
                    newval->stringval = NULL;
                    return false;
                }
            }
            break;
        case PGC_ENUM:
            {
                struct config_enum *conf = (struct config_enum *) record;

                if (!config_enum_lookup_by_name(conf, value, &newval->enumval))
                {
                    char       *hintmsg;

                    hintmsg = config_enum_get_options(conf,
                                                      "Available values: ",
                                                      ".", ", ");

                    ereport(elevel,
                            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                             errmsg("invalid value for parameter \"%s\": \"%s\"",
                                    name, value),
                             hintmsg ? errhint("%s", _(hintmsg)) : 0));

                    if (hintmsg)
                        pfree(hintmsg);
                    return false;
                }

                if (!call_enum_check_hook(conf, &newval->enumval, newextra,
                                          source, elevel))
                    return false;
            }
            break;
    }

    return true;
}


/*
 * Sets option `name' to given value.
 *
 * The value should be a string, which will be parsed and converted to
 * the appropriate data type.  The context and source parameters indicate
 * in which context this function is being called, so that it can apply the
 * access restrictions properly.
 *
 * If value is NULL, set the option to its default value (normally the
 * reset_val, but if source == PGC_S_DEFAULT we instead use the boot_val).
 *
 * action indicates whether to set the value globally in the session, locally
 * to the current top transaction, or just for the duration of a function call.
 *
 * If changeVal is false then don't really set the option but do all
 * the checks to see if it would work.
 *
 * elevel should normally be passed as zero, allowing this function to make
 * its standard choice of ereport level.  However some callers need to be
 * able to override that choice; they should pass the ereport level to use.
 *
 * Return value:
 *    +1: the value is valid and was successfully applied.
 *    0:    the name or value is invalid (but see below).
 *    -1: the value was not applied because of context, priority, or changeVal.
 *
 * If there is an error (non-existing option, invalid value) then an
 * ereport(ERROR) is thrown *unless* this is called for a source for which
 * we don't want an ERROR (currently, those are defaults, the config file,
 * and per-database or per-user settings, as well as callers who specify
 * a less-than-ERROR elevel).  In those cases we write a suitable error
 * message via ereport() and return 0.
 *
 * See also SetConfigOption for an external interface.
 */
int
set_config_option(const char *name, const char *value,
                  GucContext context, GucSource source,
                  GucAction action, bool changeVal, int elevel,
                  bool is_reload)
{// #lizard forgives
    struct config_generic *record;
    union config_var_val newval_union;
    void       *newextra = NULL;
    bool        prohibitValueChange = false;
    bool        makeDefault;

#ifdef XCP
    bool        send_to_nodes = false;

	/*
	 * Determine now, because source may be changed below in the function.
	 * remotetype and parentnode are only used in internal connections.
	 */
    if ((source == PGC_S_SESSION || source == PGC_S_CLIENT)
        && (IS_PGXC_DATANODE || !IsConnFromCoord())
        && (strcmp(name,"remotetype") != 0 && strcmp(name,"parentnode") != 0))
    {
        send_to_nodes = true;
    }
#endif

#ifdef PGXC
    /*
     * Current GucContest value is needed to check if xc_maintenance_mode parameter
     * is specified in valid contests.   It is allowed only by SET command or
     * libpq connect parameters so that setting this ON is just temporary.
     */
    currentGucContext = context;
#endif

    if (elevel == 0)
    {
        if (source == PGC_S_DEFAULT || source == PGC_S_FILE)
        {
            /*
             * To avoid cluttering the log, only the postmaster bleats loudly
             * about problems with the config file.
             */
            elevel = IsUnderPostmaster ? DEBUG3 : LOG;
        }
        else if (source == PGC_S_GLOBAL ||
                 source == PGC_S_DATABASE ||
                 source == PGC_S_USER ||
                 source == PGC_S_DATABASE_USER)
            elevel = WARNING;
        else
            elevel = ERROR;
    }

    /*
     * GUC_ACTION_SAVE changes are acceptable during a parallel operation,
     * because the current worker will also pop the change.  We're probably
     * dealing with a function having a proconfig entry.  Only the function's
     * body should observe the change, and peer workers do not share in the
     * execution of a function call started by this worker.
     *
     * Other changes might need to affect other workers, so forbid them.
     */
    if (IsInParallelMode() && changeVal && action != GUC_ACTION_SAVE)
        ereport(elevel,
                (errcode(ERRCODE_INVALID_TRANSACTION_STATE),
                 errmsg("cannot set parameters during a parallel operation")));

    record = find_option(name, true, elevel);
    if (record == NULL)
    {
        ereport(elevel,
                (errcode(ERRCODE_UNDEFINED_OBJECT),
                 errmsg("unrecognized configuration parameter \"%s\"", name)));
        return 0;
    }

    /*
     * Check if the option can be set at this time. See guc.h for the precise
     * rules.
     */
    switch (record->context)
    {
        case PGC_INTERNAL:
            if (context != PGC_INTERNAL)
            {
                ereport(elevel,
                        (errcode(ERRCODE_CANT_CHANGE_RUNTIME_PARAM),
                         errmsg("parameter \"%s\" cannot be changed",
                                name)));
                return 0;
            }
            break;
        case PGC_POSTMASTER:
            if (context == PGC_SIGHUP)
            {
                /*
                 * We are re-reading a PGC_POSTMASTER variable from
                 * postgresql.conf.  We can't change the setting, so we should
                 * give a warning if the DBA tries to change it.  However,
                 * because of variant formats, canonicalization by check
                 * hooks, etc, we can't just compare the given string directly
                 * to what's stored.  Set a flag to check below after we have
                 * the final storable value.
                 */
                prohibitValueChange = true;
            }
            else if (context != PGC_POSTMASTER)
            {
                ereport(elevel,
                        (errcode(ERRCODE_CANT_CHANGE_RUNTIME_PARAM),
                         errmsg("parameter \"%s\" cannot be changed without restarting the server",
                                name)));
                return 0;
            }
            break;
        case PGC_SIGHUP:
            if (context != PGC_SIGHUP && context != PGC_POSTMASTER)
            {
                ereport(elevel,
                        (errcode(ERRCODE_CANT_CHANGE_RUNTIME_PARAM),
                         errmsg("parameter \"%s\" cannot be changed now",
                                name)));
                return 0;
            }

            /*
             * Hmm, the idea of the SIGHUP context is "ought to be global, but
             * can be changed after postmaster start". But there's nothing
             * that prevents a crafty administrator from sending SIGHUP
             * signals to individual backends only.
             */
            break;
        case PGC_SU_BACKEND:
            /* Reject if we're connecting but user is not superuser */
            if (context == PGC_BACKEND)
            {
                ereport(elevel,
                        (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                         errmsg("permission denied to set parameter \"%s\"",
                                name)));
                return 0;
            }
            /* FALL THRU to process the same as PGC_BACKEND */
        case PGC_BACKEND:
            if (context == PGC_SIGHUP)
            {
                /*
                 * If a PGC_BACKEND or PGC_SU_BACKEND parameter is changed in
                 * the config file, we want to accept the new value in the
                 * postmaster (whence it will propagate to
                 * subsequently-started backends), but ignore it in existing
                 * backends.  This is a tad klugy, but necessary because we
                 * don't re-read the config file during backend start.
                 *
                 * In EXEC_BACKEND builds, this works differently: we load all
                 * non-default settings from the CONFIG_EXEC_PARAMS file
                 * during backend start.  In that case we must accept
                 * PGC_SIGHUP settings, so as to have the same value as if
                 * we'd forked from the postmaster.  This can also happen when
                 * using RestoreGUCState() within a background worker that
                 * needs to have the same settings as the user backend that
                 * started it. is_reload will be true when either situation
                 * applies.
                 */
                if (IsUnderPostmaster && !is_reload)
                    return -1;
            }
            else if (context != PGC_POSTMASTER &&
                     context != PGC_BACKEND &&
                     context != PGC_SU_BACKEND &&
                     source != PGC_S_CLIENT)
            {
                ereport(elevel,
                        (errcode(ERRCODE_CANT_CHANGE_RUNTIME_PARAM),
                         errmsg("parameter \"%s\" cannot be set after connection start",
                                name)));
                return 0;
            }
            break;
        case PGC_SUSET:
            if (context == PGC_USERSET || context == PGC_BACKEND)
            {
                ereport(elevel,
                        (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                         errmsg("permission denied to set parameter \"%s\"",
                                name)));
                return 0;
            }
            break;
        case PGC_USERSET:
            /* always okay */
            break;
    }

    /*
     * Disallow changing GUC_NOT_WHILE_SEC_REST values if we are inside a
     * security restriction context.  We can reject this regardless of the GUC
     * context or source, mainly because sources that it might be reasonable
     * to override for won't be seen while inside a function.
     *
     * Note: variables marked GUC_NOT_WHILE_SEC_REST should usually be marked
     * GUC_NO_RESET_ALL as well, because ResetAllOptions() doesn't check this.
     * An exception might be made if the reset value is assumed to be "safe".
     *
     * Note: this flag is currently used for "session_authorization" and
     * "role".  We need to prohibit changing these inside a local userid
     * context because when we exit it, GUC won't be notified, leaving things
     * out of sync.  (This could be fixed by forcing a new GUC nesting level,
     * but that would change behavior in possibly-undesirable ways.)  Also, we
     * prohibit changing these in a security-restricted operation because
     * otherwise RESET could be used to regain the session user's privileges.
     */
    if (record->flags & GUC_NOT_WHILE_SEC_REST)
    {
        if (InLocalUserIdChange())
        {
            /*
             * Phrasing of this error message is historical, but it's the most
             * common case.
             */
            ereport(elevel,
                    (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                     errmsg("cannot set parameter \"%s\" within security-definer function",
                            name)));
            return 0;
        }
        if (InSecurityRestrictedOperation())
        {
            ereport(elevel,
                    (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                     errmsg("cannot set parameter \"%s\" within security-restricted operation",
                            name)));
            return 0;
        }
    }

    /*
     * Should we set reset/stacked values?    (If so, the behavior is not
     * transactional.)    This is done either when we get a default value from
     * the database's/user's/client's default settings or when we reset a
     * value to its default.
     */
    makeDefault = changeVal && (source <= PGC_S_OVERRIDE) &&
        ((value != NULL) || source == PGC_S_DEFAULT);

    /*
     * Ignore attempted set if overridden by previously processed setting.
     * However, if changeVal is false then plow ahead anyway since we are
     * trying to find out if the value is potentially good, not actually use
     * it. Also keep going if makeDefault is true, since we may want to set
     * the reset/stacked values even if we can't set the variable itself.
     */
    if (record->source > source)
    {
        if (changeVal && !makeDefault)
        {
            elog(DEBUG3, "\"%s\": setting ignored because previous source is higher priority",
                 name);
            return -1;
        }
        changeVal = false;
    }

    /*
     * Evaluate value and set variable.
     */
    switch (record->vartype)
    {
        case PGC_BOOL:
            {
                struct config_bool *conf = (struct config_bool *) record;

#define newval (newval_union.boolval)

                if (value)
                {
                    if (!parse_and_validate_value(record, name, value,
                                                  source, elevel,
                                                  &newval_union, &newextra))
                        return 0;
                }
                else if (source == PGC_S_DEFAULT)
                {
                    newval = conf->boot_val;
                    if (!call_bool_check_hook(conf, &newval, &newextra,
                                              source, elevel))
                        return 0;
                }
                else
                {
                    newval = conf->reset_val;
                    newextra = conf->reset_extra;
                    source = conf->gen.reset_source;
                    context = conf->gen.reset_scontext;
                }

                if (prohibitValueChange)
                {
                    if (*conf->variable != newval)
                    {
                        record->status |= GUC_PENDING_RESTART;
                        ereport(elevel,
                                (errcode(ERRCODE_CANT_CHANGE_RUNTIME_PARAM),
                                 errmsg("parameter \"%s\" cannot be changed without restarting the server",
                                        name)));
                        return 0;
                    }
                    record->status &= ~GUC_PENDING_RESTART;
                    return -1;
                }

                if (changeVal)
                {
                    /* Save old value to support transaction abort */
                    if (!makeDefault)
                        push_old_value(&conf->gen, action);

                    if (conf->assign_hook)
                        (*conf->assign_hook) (newval, newextra);
                    *conf->variable = newval;
                    set_extra_field(&conf->gen, &conf->gen.extra,
                                    newextra);
                    conf->gen.source = source;
                    conf->gen.scontext = context;
                }
                if (makeDefault)
                {
                    GucStack   *stack;

                    if (conf->gen.reset_source <= source)
                    {
                        conf->reset_val = newval;
                        set_extra_field(&conf->gen, &conf->reset_extra,
                                        newextra);
                        conf->gen.reset_source = source;
                        conf->gen.reset_scontext = context;
                    }
                    for (stack = conf->gen.stack; stack; stack = stack->prev)
                    {
                        if (stack->source <= source)
                        {
                            stack->prior.val.boolval = newval;
                            set_extra_field(&conf->gen, &stack->prior.extra,
                                            newextra);
                            stack->source = source;
                            stack->scontext = context;
                        }
                    }
                }

                /* Perhaps we didn't install newextra anywhere */
                if (newextra && !extra_field_used(&conf->gen, newextra))
                    free(newextra);
                break;

#undef newval
            }

        case PGC_INT:
            {
                struct config_int *conf = (struct config_int *) record;

#define newval (newval_union.intval)

                if (value)
                {
                    if (!parse_and_validate_value(record, name, value,
                                                  source, elevel,
                                                  &newval_union, &newextra))
                        return 0;
                }
                else if (source == PGC_S_DEFAULT)
                {
                    newval = conf->boot_val;
                    if (!call_int_check_hook(conf, &newval, &newextra,
                                             source, elevel))
                        return 0;
                }
                else
                {
                    newval = conf->reset_val;
                    newextra = conf->reset_extra;
                    source = conf->gen.reset_source;
                    context = conf->gen.reset_scontext;
                }

                if (prohibitValueChange)
                {
                    if (*conf->variable != newval)
                    {
                        record->status |= GUC_PENDING_RESTART;
                        ereport(elevel,
                                (errcode(ERRCODE_CANT_CHANGE_RUNTIME_PARAM),
                                 errmsg("parameter \"%s\" cannot be changed without restarting the server",
                                        name)));
                        return 0;
                    }
                    record->status &= ~GUC_PENDING_RESTART;
                    return -1;
                }

                if (changeVal)
                {
                    /* Save old value to support transaction abort */
                    if (!makeDefault)
                        push_old_value(&conf->gen, action);

                    if (conf->assign_hook)
                        (*conf->assign_hook) (newval, newextra);
                    *conf->variable = newval;
                    set_extra_field(&conf->gen, &conf->gen.extra,
                                    newextra);
                    conf->gen.source = source;
                    conf->gen.scontext = context;
                }
                if (makeDefault)
                {
                    GucStack   *stack;

                    if (conf->gen.reset_source <= source)
                    {
                        conf->reset_val = newval;
                        set_extra_field(&conf->gen, &conf->reset_extra,
                                        newextra);
                        conf->gen.reset_source = source;
                        conf->gen.reset_scontext = context;
                    }
                    for (stack = conf->gen.stack; stack; stack = stack->prev)
                    {
                        if (stack->source <= source)
                        {
                            stack->prior.val.intval = newval;
                            set_extra_field(&conf->gen, &stack->prior.extra,
                                            newextra);
                            stack->source = source;
                            stack->scontext = context;
                        }
                    }
                }

                /* Perhaps we didn't install newextra anywhere */
                if (newextra && !extra_field_used(&conf->gen, newextra))
                    free(newextra);
                break;

#undef newval
            }

        case PGC_UINT:
            {
                struct config_uint *conf = (struct config_uint *) record;

#define newval (newval_union.uintval)

                if (value)
                {
                    if (!parse_and_validate_value(record, name, value,
                                                  source, elevel,
                                                  &newval_union, &newextra))
                        return 0;
                }
                else if (source == PGC_S_DEFAULT)
                {
                    newval = conf->boot_val;
                    if (!call_uint_check_hook(conf, &newval, &newextra,
                                              source, elevel))
                        return 0;
                }
                else
                {
                    newval = conf->reset_val;
                    newextra = conf->reset_extra;
                    source = conf->gen.reset_source;
                    context = conf->gen.reset_scontext;
                }

                if (prohibitValueChange)
                {
                    if (*conf->variable != newval)
                    {
                        record->status |= GUC_PENDING_RESTART;
                        ereport(elevel,
                                (errcode(ERRCODE_CANT_CHANGE_RUNTIME_PARAM),
                                 errmsg("parameter \"%s\" cannot be changed without restarting the server",
                                        name)));
                        return 0;
                    }
                    record->status &= ~GUC_PENDING_RESTART;
                    return -1;
                }

                if (changeVal)
                {
                    /* Save old value to support transaction abort */
                    if (!makeDefault)
                        push_old_value(&conf->gen, action);

                    if (conf->assign_hook)
                        (*conf->assign_hook) (newval, newextra);
                    *conf->variable = newval;
                    set_extra_field(&conf->gen, &conf->gen.extra,
                                    newextra);
                    conf->gen.source = source;
                    conf->gen.scontext = context;
                }
                if (makeDefault)
                {
                    GucStack   *stack;

                    if (conf->gen.reset_source <= source)
                    {
                        conf->reset_val = newval;
                        set_extra_field(&conf->gen, &conf->reset_extra,
                                        newextra);
                        conf->gen.reset_source = source;
                        conf->gen.reset_scontext = context;
                    }
                    for (stack = conf->gen.stack; stack; stack = stack->prev)
                    {
                        if (stack->source <= source)
                        {
                            stack->prior.val.uintval = newval;
                            set_extra_field(&conf->gen, &stack->prior.extra,
                                            newextra);
                            stack->source = source;
                            stack->scontext = context;
                        }
                    }
                }

                /* Perhaps we didn't install newextra anywhere */
                if (newextra && !extra_field_used(&conf->gen, newextra))
                    free(newextra);
                break;

#undef newval
            }

        case PGC_REAL:
            {
                struct config_real *conf = (struct config_real *) record;

#define newval (newval_union.realval)

                if (value)
                {
                    if (!parse_and_validate_value(record, name, value,
                                                  source, elevel,
                                                  &newval_union, &newextra))
                        return 0;
                }
                else if (source == PGC_S_DEFAULT)
                {
                    newval = conf->boot_val;
                    if (!call_real_check_hook(conf, &newval, &newextra,
                                              source, elevel))
                        return 0;
                }
                else
                {
                    newval = conf->reset_val;
                    newextra = conf->reset_extra;
                    source = conf->gen.reset_source;
                    context = conf->gen.reset_scontext;
                }

                if (prohibitValueChange)
                {
                    if (*conf->variable != newval)
                    {
                        record->status |= GUC_PENDING_RESTART;
                        ereport(elevel,
                                (errcode(ERRCODE_CANT_CHANGE_RUNTIME_PARAM),
                                 errmsg("parameter \"%s\" cannot be changed without restarting the server",
                                        name)));
                        return 0;
                    }
                    record->status &= ~GUC_PENDING_RESTART;
                    return -1;
                }

                if (changeVal)
                {
                    /* Save old value to support transaction abort */
                    if (!makeDefault)
                        push_old_value(&conf->gen, action);

                    if (conf->assign_hook)
                        (*conf->assign_hook) (newval, newextra);
                    *conf->variable = newval;
                    set_extra_field(&conf->gen, &conf->gen.extra,
                                    newextra);
                    conf->gen.source = source;
                    conf->gen.scontext = context;
                }
                if (makeDefault)
                {
                    GucStack   *stack;

                    if (conf->gen.reset_source <= source)
                    {
                        conf->reset_val = newval;
                        set_extra_field(&conf->gen, &conf->reset_extra,
                                        newextra);
                        conf->gen.reset_source = source;
                        conf->gen.reset_scontext = context;
                    }
                    for (stack = conf->gen.stack; stack; stack = stack->prev)
                    {
                        if (stack->source <= source)
                        {
                            stack->prior.val.realval = newval;
                            set_extra_field(&conf->gen, &stack->prior.extra,
                                            newextra);
                            stack->source = source;
                            stack->scontext = context;
                        }
                    }
                }

                /* Perhaps we didn't install newextra anywhere */
                if (newextra && !extra_field_used(&conf->gen, newextra))
                    free(newextra);
                break;

#undef newval
            }

        case PGC_STRING:
            {
                struct config_string *conf = (struct config_string *) record;

#define newval (newval_union.stringval)

                if (value)
                {
                    if (!parse_and_validate_value(record, name, value,
                                                  source, elevel,
                                                  &newval_union, &newextra))
                        return 0;
                }
                else if (source == PGC_S_DEFAULT)
                {
                    /* non-NULL boot_val must always get strdup'd */
                    if (conf->boot_val != NULL)
                    {
                        newval = guc_strdup(elevel, conf->boot_val);
                        if (newval == NULL)
                            return 0;
                    }
                    else
                        newval = NULL;

                    if (!call_string_check_hook(conf, &newval, &newextra,
                                                source, elevel))
                    {
                        free(newval);
                        return 0;
                    }
                }
                else
                {
                    /*
                     * strdup not needed, since reset_val is already under
                     * guc.c's control
                     */
                    newval = conf->reset_val;
                    newextra = conf->reset_extra;
                    source = conf->gen.reset_source;
                    context = conf->gen.reset_scontext;
                }

                if (prohibitValueChange)
                {
                    /* newval shouldn't be NULL, so we're a bit sloppy here */
                    if (*conf->variable == NULL || newval == NULL ||
                        strcmp(*conf->variable, newval) != 0)
                    {
                        record->status |= GUC_PENDING_RESTART;
                        ereport(elevel,
                                (errcode(ERRCODE_CANT_CHANGE_RUNTIME_PARAM),
                                 errmsg("parameter \"%s\" cannot be changed without restarting the server",
                                        name)));
                        return 0;
                    }
                    record->status &= ~GUC_PENDING_RESTART;
                    return -1;
                }

                if (changeVal)
                {
                    /* Save old value to support transaction abort */
                    if (!makeDefault)
                        push_old_value(&conf->gen, action);

                    if (conf->assign_hook)
                        (*conf->assign_hook) (newval, newextra);
                    set_string_field(conf, conf->variable, newval);
                    set_extra_field(&conf->gen, &conf->gen.extra,
                                    newextra);
                    conf->gen.source = source;
                    conf->gen.scontext = context;
                }

                if (makeDefault)
                {
                    GucStack   *stack;

                    if (conf->gen.reset_source <= source)
                    {
                        set_string_field(conf, &conf->reset_val, newval);
                        set_extra_field(&conf->gen, &conf->reset_extra,
                                        newextra);
                        conf->gen.reset_source = source;
                        conf->gen.reset_scontext = context;
                    }
                    for (stack = conf->gen.stack; stack; stack = stack->prev)
                    {
                        if (stack->source <= source)
                        {
                            set_string_field(conf, &stack->prior.val.stringval,
                                             newval);
                            set_extra_field(&conf->gen, &stack->prior.extra,
                                            newextra);
                            stack->source = source;
                            stack->scontext = context;
                        }
                    }
                }

                /* Perhaps we didn't install newval anywhere */
                if (newval && !string_field_used(conf, newval))
                    free(newval);
                /* Perhaps we didn't install newextra anywhere */
                if (newextra && !extra_field_used(&conf->gen, newextra))
                    free(newextra);
                break;

#undef newval
            }

        case PGC_ENUM:
            {
                struct config_enum *conf = (struct config_enum *) record;

#define newval (newval_union.enumval)

                if (value)
                {
                    if (!parse_and_validate_value(record, name, value,
                                                  source, elevel,
                                                  &newval_union, &newextra))
                        return 0;
                }
                else if (source == PGC_S_DEFAULT)
                {
                    newval = conf->boot_val;
                    if (!call_enum_check_hook(conf, &newval, &newextra,
                                              source, elevel))
                        return 0;
                }
                else
                {
                    newval = conf->reset_val;
                    newextra = conf->reset_extra;
                    source = conf->gen.reset_source;
                    context = conf->gen.reset_scontext;
                }

                if (prohibitValueChange)
                {
                    if (*conf->variable != newval)
                    {
                        record->status |= GUC_PENDING_RESTART;
                        ereport(elevel,
                                (errcode(ERRCODE_CANT_CHANGE_RUNTIME_PARAM),
                                 errmsg("parameter \"%s\" cannot be changed without restarting the server",
                                        name)));
                        return 0;
                    }
                    record->status &= ~GUC_PENDING_RESTART;
                    return -1;
                }

                if (changeVal)
                {
                    /* Save old value to support transaction abort */
                    if (!makeDefault)
                        push_old_value(&conf->gen, action);

                    if (conf->assign_hook)
                        (*conf->assign_hook) (newval, newextra);
                    *conf->variable = newval;
                    set_extra_field(&conf->gen, &conf->gen.extra,
                                    newextra);
                    conf->gen.source = source;
                    conf->gen.scontext = context;
                }
                if (makeDefault)
                {
                    GucStack   *stack;

                    if (conf->gen.reset_source <= source)
                    {
                        conf->reset_val = newval;
                        set_extra_field(&conf->gen, &conf->reset_extra,
                                        newextra);
                        conf->gen.reset_source = source;
                        conf->gen.reset_scontext = context;
                    }
                    for (stack = conf->gen.stack; stack; stack = stack->prev)
                    {
                        if (stack->source <= source)
                        {
                            stack->prior.val.enumval = newval;
                            set_extra_field(&conf->gen, &stack->prior.extra,
                                            newextra);
                            stack->source = source;
                            stack->scontext = context;
                        }
                    }
                }

                /* Perhaps we didn't install newextra anywhere */
                if (newextra && !extra_field_used(&conf->gen, newextra))
                    free(newextra);
                break;

#undef newval
            }
    }

    if (changeVal && (record->flags & GUC_REPORT))
        ReportGUCOption(record);

#ifdef XCP
    if (send_to_nodes)
    {
        RemoteQuery    *step;
        StringInfoData     poolcmd;

        initStringInfo(&poolcmd);
        /*
         * Save new parameter value with the node manager.
         * XXX here we may check: if value equals to configuration default
         * just reset parameter instead. Minus one table entry, shorter SET
		 * command sent down... Sounds like optimization.
         */

        if (action == GUC_ACTION_LOCAL)
        {
            if (IsTransactionBlock())
                PGXCNodeSetParam(true, name, value, record->flags);
            value = quote_guc_value(value, record->flags);
            appendStringInfo(&poolcmd, "SET LOCAL %s TO %s", name,
                    (value ? value : "DEFAULT"));
        }
        else
        {
            PGXCNodeSetParam(false, name, value, record->flags);
            value = quote_guc_value(value, record->flags);
            appendStringInfo(&poolcmd, "SET %s TO %s", name,
                    (value ? value : "DEFAULT"));
        }
        /*
         * Send new value down to remote nodes if any is connected
         * XXX here we are creatig a node and invoke a function that is trying
         * to send some. That introduces some overhead, which may seem to be
         * significant if application sets a bunch of parameters before doing
         * anything useful - waste work for for each set statement.
         * We may want to avoid that, by resetting the remote parameters and
         * flagging that parameters needs to be updated before sending down next
         * statement.
         * On the other hand if session runs with a number of customized
         * parameters and switching one, that would cause all values are resent.
         * So let's go with "send immediately" approach: parameters are not set
         * too often to care about overhead here.
         */
        step = makeNode(RemoteQuery);
        step->combine_type = COMBINE_TYPE_SAME;
        step->exec_nodes = NULL;
        step->sql_statement = poolcmd.data;
        /* force_autocommit is actually does not start transaction on nodes */
        step->force_autocommit = true;
        step->exec_type = EXEC_ON_CURRENT;
		step->is_set = true;
        ExecRemoteUtility(step);
        pfree(step);
        pfree(poolcmd.data);
    }
#endif


#if 0
#ifdef XCP
    if (send_to_nodes)
    {
#if 0
        RemoteQuery    *step;
        StringInfoData     poolcmd;

        initStringInfo(&poolcmd);
#endif
        /*
         * Save new parameter value with the node manager.
         * XXX here we may check: if value equals to configuration default
         * just reset parameter instead. Minus one table entry, shorter SET
         * command sent downn... Sounds like optimization.
         */

        if (action == GUC_ACTION_LOCAL)
        {
            if (IsTransactionBlock())
                PGXCNodeSetParam(true, name, value, record->flags);
#if 0
            value = quote_guc_value(value, record->flags);
            appendStringInfo(&poolcmd, "SET LOCAL %s TO %s", name,
                    (value ? value : "DEFAULT"));
#endif
        }
        else
        {
            PGXCNodeSetParam(false, name, value, record->flags);
#if 0
            value = quote_guc_value(value, record->flags);
            appendStringInfo(&poolcmd, "SET %s TO %s", name,
                    (value ? value : "DEFAULT"));
#endif
        }
#if 0
        /*
         * Send new value down to remote nodes if any is connected
         * XXX here we are creatig a node and invoke a function that is trying
         * to send some. That introduces some overhead, which may seem to be
         * significant if application sets a bunch of parameters before doing
         * anything useful - waste work for for each set statement.
         * We may want to avoid that, by resetting the remote parameters and
         * flagging that parameters needs to be updated before sending down next
         * statement.
         * On the other hand if session runs with a number of customized
         * parameters and switching one, that would cause all values are resent.
         * So let's go with "send immediately" approach: parameters are not set
         * too often to care about overhead here.
         */
        step = makeNode(RemoteQuery);
        step->combine_type = COMBINE_TYPE_SAME;
        step->exec_nodes = NULL;
        step->sql_statement = poolcmd.data;
        /* force_autocommit is actually does not start transaction on nodes */
        step->force_autocommit = true;
        step->exec_type = EXEC_ON_CURRENT;
        ExecRemoteUtility(step);
        pfree(step);
        pfree(poolcmd.data);
#endif
    }
#endif
#endif

    return changeVal ? 1 : -1;
}


/*
 * Set the fields for source file and line number the setting came from.
 */
static void
set_config_sourcefile(const char *name, char *sourcefile, int sourceline)
{
    struct config_generic *record;
    int            elevel;

    /*
     * To avoid cluttering the log, only the postmaster bleats loudly about
     * problems with the config file.
     */
    elevel = IsUnderPostmaster ? DEBUG3 : LOG;

    record = find_option(name, true, elevel);
    /* should not happen */
    if (record == NULL)
        elog(ERROR, "unrecognized configuration parameter \"%s\"", name);

    sourcefile = guc_strdup(elevel, sourcefile);
    if (record->sourcefile)
        free(record->sourcefile);
    record->sourcefile = sourcefile;
    record->sourceline = sourceline;
}

/*
 * Set a config option to the given value.
 *
 * See also set_config_option; this is just the wrapper to be called from
 * outside GUC.  (This function should be used when possible, because its API
 * is more stable than set_config_option's.)
 *
 * Note: there is no support here for setting source file/line, as it
 * is currently not needed.
 */
void
SetConfigOption(const char *name, const char *value,
                GucContext context, GucSource source)
{
    (void) set_config_option(name, value, context, source,
                             GUC_ACTION_SET, true, 0, false);
}



/*
 * Fetch the current value of the option `name', as a string.
 *
 * If the option doesn't exist, return NULL if missing_ok is true (NOTE that
 * this cannot be distinguished from a string variable with a NULL value!),
 * otherwise throw an ereport and don't return.
 *
 * If restrict_superuser is true, we also enforce that only superusers can
 * see GUC_SUPERUSER_ONLY variables.  This should only be passed as true
 * in user-driven calls.
 *
 * The string is *not* allocated for modification and is really only
 * valid until the next call to configuration related functions.
 */
const char *
GetConfigOption(const char *name, bool missing_ok, bool restrict_superuser)
{// #lizard forgives
    struct config_generic *record;
    static char buffer[256];

    record = find_option(name, false, ERROR);
    if (record == NULL)
    {
        if (missing_ok)
            return NULL;
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_OBJECT),
                 errmsg("unrecognized configuration parameter \"%s\"",
                        name)));
    }
    if (restrict_superuser &&
        (record->flags & GUC_SUPERUSER_ONLY) &&
        !is_member_of_role(GetUserId(), DEFAULT_ROLE_READ_ALL_SETTINGS))
        ereport(ERROR,
                (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                 errmsg("must be superuser or a member of pg_read_all_settings to examine \"%s\"",
                        name)));

    switch (record->vartype)
    {
        case PGC_BOOL:
            return *((struct config_bool *) record)->variable ? "on" : "off";

        case PGC_INT:
            snprintf(buffer, sizeof(buffer), "%d",
                     *((struct config_int *) record)->variable);
            return buffer;

        case PGC_UINT:
            snprintf(buffer, sizeof(buffer), "%u",
                     *((struct config_int *) record)->variable);
            return buffer;

        case PGC_REAL:
            snprintf(buffer, sizeof(buffer), "%g",
                     *((struct config_real *) record)->variable);
            return buffer;

        case PGC_STRING:
            return *((struct config_string *) record)->variable;

        case PGC_ENUM:
            return config_enum_lookup_by_value((struct config_enum *) record,
                                               *((struct config_enum *) record)->variable);
    }
    return NULL;
}

/*
 * Get the RESET value associated with the given option.
 *
 * Note: this is not re-entrant, due to use of static result buffer;
 * not to mention that a string variable could have its reset_val changed.
 * Beware of assuming the result value is good for very long.
 */
const char *
GetConfigOptionResetString(const char *name)
{// #lizard forgives
    struct config_generic *record;
    static char buffer[256];

    record = find_option(name, false, ERROR);
    if (record == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_OBJECT),
                 errmsg("unrecognized configuration parameter \"%s\"", name)));
    if ((record->flags & GUC_SUPERUSER_ONLY) &&
        !is_member_of_role(GetUserId(), DEFAULT_ROLE_READ_ALL_SETTINGS))
        ereport(ERROR,
                (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                 errmsg("must be superuser or a member of pg_read_all_settings to examine \"%s\"",
                        name)));

    switch (record->vartype)
    {
        case PGC_BOOL:
            return ((struct config_bool *) record)->reset_val ? "on" : "off";

        case PGC_INT:
            snprintf(buffer, sizeof(buffer), "%d",
                     ((struct config_int *) record)->reset_val);
            return buffer;

        case PGC_UINT:
            snprintf(buffer, sizeof(buffer), "%u",
                     ((struct config_int *) record)->reset_val);
            return buffer;

        case PGC_REAL:
            snprintf(buffer, sizeof(buffer), "%g",
                     ((struct config_real *) record)->reset_val);
            return buffer;

        case PGC_STRING:
            return ((struct config_string *) record)->reset_val;

        case PGC_ENUM:
            return config_enum_lookup_by_value((struct config_enum *) record,
                                               ((struct config_enum *) record)->reset_val);
    }
    return NULL;
}


/*
 * flatten_set_variable_args
 *        Given a parsenode List as emitted by the grammar for SET,
 *        convert to the flat string representation used by GUC.
 *
 * We need to be told the name of the variable the args are for, because
 * the flattening rules vary (ugh).
 *
 * The result is NULL if args is NIL (i.e., SET ... TO DEFAULT), otherwise
 * a palloc'd string.
 */
static char *
flatten_set_variable_args(const char *name, List *args)
{// #lizard forgives
    struct config_generic *record;
    int            flags;
    StringInfoData buf;
    ListCell   *l;

    /* Fast path if just DEFAULT */
    if (args == NIL)
        return NULL;

    /*
     * Get flags for the variable; if it's not known, use default flags.
     * (Caller might throw error later, but not our business to do so here.)
     */
    record = find_option(name, false, WARNING);
    if (record)
        flags = record->flags;
    else
        flags = 0;

    /* Complain if list input and non-list variable */
    if ((flags & GUC_LIST_INPUT) == 0 &&
        list_length(args) != 1)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("SET %s takes only one argument", name)));

    initStringInfo(&buf);

    /*
     * Each list member may be a plain A_Const node, or an A_Const within a
     * TypeCast; the latter case is supported only for ConstInterval arguments
     * (for SET TIME ZONE).
     */
    foreach(l, args)
    {
        Node       *arg = (Node *) lfirst(l);
        char       *val;
        TypeName   *typeName = NULL;
        A_Const    *con;

        if (l != list_head(args))
            appendStringInfoString(&buf, ", ");

        if (IsA(arg, TypeCast))
        {
            TypeCast   *tc = (TypeCast *) arg;

            arg = tc->arg;
            typeName = tc->typeName;
        }

        if (!IsA(arg, A_Const))
            elog(ERROR, "unrecognized node type: %d", (int) nodeTag(arg));
        con = (A_Const *) arg;

        switch (nodeTag(&con->val))
        {
            case T_Integer:
                appendStringInfo(&buf, "%ld", intVal(&con->val));
                break;
            case T_Float:
                /* represented as a string, so just copy it */
                appendStringInfoString(&buf, strVal(&con->val));
                break;
            case T_String:
                val = strVal(&con->val);
                if (typeName != NULL)
                {
                    /*
                     * Must be a ConstInterval argument for TIME ZONE. Coerce
                     * to interval and back to normalize the value and account
                     * for any typmod.
                     */
                    Oid            typoid;
                    int32        typmod;
                    Datum        interval;
                    char       *intervalout;

                    typenameTypeIdAndMod(NULL, typeName, &typoid, &typmod);
                    Assert(typoid == INTERVALOID);

                    interval =
                        DirectFunctionCall3(interval_in,
                                            CStringGetDatum(val),
                                            ObjectIdGetDatum(InvalidOid),
                                            Int32GetDatum(typmod));

                    intervalout =
                        DatumGetCString(DirectFunctionCall1(interval_out,
                                                            interval));
                    appendStringInfo(&buf, "INTERVAL '%s'", intervalout);
                }
                else
                {
                    /*
                     * Plain string literal or identifier.  For quote mode,
                     * quote it if it's not a vanilla identifier.
                     */
                    if (flags & GUC_LIST_QUOTE)
                        appendStringInfoString(&buf, quote_identifier(val));
                    else
                        appendStringInfoString(&buf, val);
                }
                break;
            default:
                elog(ERROR, "unrecognized node type: %d",
                     (int) nodeTag(&con->val));
                break;
        }
    }

    return buf.data;
}

/*
 * Write updated configuration parameter values into a temporary file.
 * This function traverses the list of parameters and quotes the string
 * values before writing them.
 */
static void
write_auto_conf_file(int fd, const char *filename, ConfigVariable *head)
{
    StringInfoData buf;
    ConfigVariable *item;

    initStringInfo(&buf);

    /* Emit file header containing warning comment */
    appendStringInfoString(&buf, "# Do not edit this file manually!\n");
    appendStringInfoString(&buf, "# It will be overwritten by ALTER SYSTEM command.\n");

    errno = 0;
    if (write(fd, buf.data, buf.len) != buf.len)
    {
        /* if write didn't set errno, assume problem is no disk space */
        if (errno == 0)
            errno = ENOSPC;
        ereport(ERROR,
                (errcode_for_file_access(),
                 errmsg("could not write to file \"%s\": %m", filename)));
    }

    /* Emit each parameter, properly quoting the value */
    for (item = head; item != NULL; item = item->next)
    {
        char       *escaped;

        resetStringInfo(&buf);

        appendStringInfoString(&buf, item->name);
        appendStringInfoString(&buf, " = '");

        escaped = escape_single_quotes_ascii(item->value);
        if (!escaped)
            ereport(ERROR,
                    (errcode(ERRCODE_OUT_OF_MEMORY),
                     errmsg("out of memory")));
        appendStringInfoString(&buf, escaped);
        free(escaped);

        appendStringInfoString(&buf, "'\n");

        errno = 0;
        if (write(fd, buf.data, buf.len) != buf.len)
        {
            /* if write didn't set errno, assume problem is no disk space */
            if (errno == 0)
                errno = ENOSPC;
            ereport(ERROR,
                    (errcode_for_file_access(),
                     errmsg("could not write to file \"%s\": %m", filename)));
        }
    }

    /* fsync before considering the write to be successful */
    if (pg_fsync(fd) != 0)
        ereport(ERROR,
                (errcode_for_file_access(),
                 errmsg("could not fsync file \"%s\": %m", filename)));

    pfree(buf.data);
}

/*
 * Update the given list of configuration parameters, adding, replacing
 * or deleting the entry for item "name" (delete if "value" == NULL).
 */
static void
replace_auto_config_value(ConfigVariable **head_p, ConfigVariable **tail_p,
                          const char *name, const char *value)
{
    ConfigVariable *item,
               *prev = NULL;

    /* Search the list for an existing match (we assume there's only one) */
    for (item = *head_p; item != NULL; item = item->next)
    {
        if (strcmp(item->name, name) == 0)
        {
            /* found a match, replace it */
            pfree(item->value);
            if (value != NULL)
            {
                /* update the parameter value */
                item->value = pstrdup(value);
            }
            else
            {
                /* delete the configuration parameter from list */
                if (*head_p == item)
                    *head_p = item->next;
                else
                    prev->next = item->next;
                if (*tail_p == item)
                    *tail_p = prev;

                pfree(item->name);
                pfree(item->filename);
                pfree(item);
            }
            return;
        }
        prev = item;
    }

    /* Not there; no work if we're trying to delete it */
    if (value == NULL)
        return;

    /* OK, append a new entry */
    item = palloc(sizeof *item);
    item->name = pstrdup(name);
    item->value = pstrdup(value);
    item->errmsg = NULL;
    item->filename = pstrdup("");    /* new item has no location */
    item->sourceline = 0;
    item->ignore = false;
    item->applied = false;
    item->next = NULL;

    if (*head_p == NULL)
        *head_p = item;
    else
        (*tail_p)->next = item;
    *tail_p = item;
}


/*
 * Execute ALTER SYSTEM statement.
 *
 * Read the old PG_AUTOCONF_FILENAME file, merge in the new variable value,
 * and write out an updated file.  If the command is ALTER SYSTEM RESET ALL,
 * we can skip reading the old file and just write an empty file.
 *
 * An LWLock is used to serialize updates of the configuration file.
 *
 * In case of an error, we leave the original automatic
 * configuration file (PG_AUTOCONF_FILENAME) intact.
 */
void
AlterSystemSetConfigFile(AlterSystemStmt *altersysstmt)
{// #lizard forgives
    char       *name;
    char       *value;
    bool        resetall = false;
    ConfigVariable *head = NULL;
    ConfigVariable *tail = NULL;
    volatile int Tmpfd;
    char        AutoConfFileName[MAXPGPATH];
    char        AutoConfTmpFileName[MAXPGPATH];

    if (!superuser())
        ereport(ERROR,
                (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                 (errmsg("must be superuser to execute ALTER SYSTEM command"))));

    /*
     * Extract statement arguments
     */
    name = altersysstmt->setstmt->name;

    switch (altersysstmt->setstmt->kind)
    {
        case VAR_SET_VALUE:
            value = ExtractSetVariableArgs(altersysstmt->setstmt);
            break;

        case VAR_SET_DEFAULT:
        case VAR_RESET:
            value = NULL;
            break;

        case VAR_RESET_ALL:
            value = NULL;
            resetall = true;
            break;

        default:
            elog(ERROR, "unrecognized alter system stmt type: %d",
                 altersysstmt->setstmt->kind);
            break;
    }

    /*
     * Unless it's RESET_ALL, validate the target variable and value
     */
    if (!resetall)
    {
        struct config_generic *record;

        record = find_option(name, false, ERROR);
        if (record == NULL)
            ereport(ERROR,
                    (errcode(ERRCODE_UNDEFINED_OBJECT),
                     errmsg("unrecognized configuration parameter \"%s\"",
                            name)));

        /*
         * Don't allow parameters that can't be set in configuration files to
         * be set in PG_AUTOCONF_FILENAME file.
         */
        if ((record->context == PGC_INTERNAL) ||
            (record->flags & GUC_DISALLOW_IN_FILE) ||
            (record->flags & GUC_DISALLOW_IN_AUTO_FILE))
            ereport(ERROR,
                    (errcode(ERRCODE_CANT_CHANGE_RUNTIME_PARAM),
                     errmsg("parameter \"%s\" cannot be changed",
                            name)));

        /*
         * If a value is specified, verify that it's sane.
         */
        if (value)
        {
            union config_var_val newval;
            void       *newextra = NULL;

            /* Check that it's acceptable for the indicated parameter */
            if (!parse_and_validate_value(record, name, value,
                                          PGC_S_FILE, ERROR,
                                          &newval, &newextra))
                ereport(ERROR,
                        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                         errmsg("invalid value for parameter \"%s\": \"%s\"",
                                name, value)));

            if (record->vartype == PGC_STRING && newval.stringval != NULL)
                free(newval.stringval);
            if (newextra)
                free(newextra);

            /*
             * We must also reject values containing newlines, because the
             * grammar for config files doesn't support embedded newlines in
             * string literals.
             */
            if (strchr(value, '\n'))
                ereport(ERROR,
                        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                         errmsg("parameter value for ALTER SYSTEM must not contain a newline")));
        }
    }

    /*
     * PG_AUTOCONF_FILENAME and its corresponding temporary file are always in
     * the data directory, so we can reference them by simple relative paths.
     */
    snprintf(AutoConfFileName, sizeof(AutoConfFileName), "%s",
             PG_AUTOCONF_FILENAME);
    snprintf(AutoConfTmpFileName, sizeof(AutoConfTmpFileName), "%s.%s",
             AutoConfFileName,
             "tmp");

    /*
     * Only one backend is allowed to operate on PG_AUTOCONF_FILENAME at a
     * time.  Use AutoFileLock to ensure that.  We must hold the lock while
     * reading the old file contents.
     */
    LWLockAcquire(AutoFileLock, LW_EXCLUSIVE);

    /*
     * If we're going to reset everything, then no need to open or parse the
     * old file.  We'll just write out an empty list.
     */
    if (!resetall)
    {
        struct stat st;

        if (stat(AutoConfFileName, &st) == 0)
        {
            /* open old file PG_AUTOCONF_FILENAME */
            FILE       *infile;

            infile = AllocateFile(AutoConfFileName, "r");
            if (infile == NULL)
                ereport(ERROR,
                        (errcode_for_file_access(),
                         errmsg("could not open file \"%s\": %m",
                                AutoConfFileName)));

            /* parse it */
            if (!ParseConfigFp(infile, AutoConfFileName, 0, LOG, &head, &tail))
                ereport(ERROR,
                        (errcode(ERRCODE_CONFIG_FILE_ERROR),
                         errmsg("could not parse contents of file \"%s\"",
                                AutoConfFileName)));

            FreeFile(infile);
        }

        /*
         * Now, replace any existing entry with the new value, or add it if
         * not present.
         */
        replace_auto_config_value(&head, &tail, name, value);
    }

    /*
     * To ensure crash safety, first write the new file data to a temp file,
     * then atomically rename it into place.
     *
     * If there is a temp file left over due to a previous crash, it's okay to
     * truncate and reuse it.
     */
    Tmpfd = BasicOpenFile(AutoConfTmpFileName,
                          O_CREAT | O_RDWR | O_TRUNC,
                          S_IRUSR | S_IWUSR);
    if (Tmpfd < 0)
        ereport(ERROR,
                (errcode_for_file_access(),
                 errmsg("could not open file \"%s\": %m",
                        AutoConfTmpFileName)));

    /*
     * Use a TRY block to clean up the file if we fail.  Since we need a TRY
     * block anyway, OK to use BasicOpenFile rather than OpenTransientFile.
     */
    PG_TRY();
    {
        /* Write and sync the new contents to the temporary file */
        write_auto_conf_file(Tmpfd, AutoConfTmpFileName, head);

        /* Close before renaming; may be required on some platforms */
        close(Tmpfd);
        Tmpfd = -1;

        /*
         * As the rename is atomic operation, if any problem occurs after this
         * at worst it can lose the parameters set by last ALTER SYSTEM
         * command.
         */
        durable_rename(AutoConfTmpFileName, AutoConfFileName, ERROR);
    }
    PG_CATCH();
    {
        /* Close file first, else unlink might fail on some platforms */
        if (Tmpfd >= 0)
            close(Tmpfd);

        /* Unlink, but ignore any error */
        (void) unlink(AutoConfTmpFileName);

        PG_RE_THROW();
    }
    PG_END_TRY();

    FreeConfigVariables(head);

    LWLockRelease(AutoFileLock);
}

/*
 * SET command
 */
void
ExecSetVariableStmt(VariableSetStmt *stmt, bool isTopLevel)
{// #lizard forgives
    GucAction    action = stmt->is_local ? GUC_ACTION_LOCAL : GUC_ACTION_SET;

    /*
     * Workers synchronize these parameters at the start of the parallel
     * operation; then, we block SET during the operation.
     */
    if (IsInParallelMode())
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_TRANSACTION_STATE),
                 errmsg("cannot set parameters during a parallel operation")));

    switch (stmt->kind)
    {
        case VAR_SET_VALUE:
        case VAR_SET_CURRENT:
            if (stmt->is_local)
                WarnNoTransactionChain(isTopLevel, "SET LOCAL");
            (void) set_config_option(stmt->name,
                                     ExtractSetVariableArgs(stmt),
                                     (superuser() ? PGC_SUSET : PGC_USERSET),
                                     PGC_S_SESSION,
                                     action, true, 0, false);
            break;
        case VAR_SET_MULTI:

            /*
             * Special-case SQL syntaxes.  The TRANSACTION and SESSION
             * CHARACTERISTICS cases effectively set more than one variable
             * per statement.  TRANSACTION SNAPSHOT only takes one argument,
             * but we put it here anyway since it's a special case and not
             * related to any GUC variable.
             */
            if (strcmp(stmt->name, "TRANSACTION") == 0)
            {
                ListCell   *head;

#ifdef XCP
                /* SET TRANSACTION assumes "local" */
                stmt->is_local = true;
#endif
                WarnNoTransactionChain(isTopLevel, "SET TRANSACTION");

                foreach(head, stmt->args)
                {
                    DefElem    *item = (DefElem *) lfirst(head);

                    if (strcmp(item->defname, "transaction_isolation") == 0)
                        SetPGVariable("transaction_isolation",
                                      list_make1(item->arg), stmt->is_local);
                    else if (strcmp(item->defname, "transaction_read_only") == 0)
                        SetPGVariable("transaction_read_only",
                                      list_make1(item->arg), stmt->is_local);
                    else if (strcmp(item->defname, "transaction_deferrable") == 0)
                        SetPGVariable("transaction_deferrable",
                                      list_make1(item->arg), stmt->is_local);
                    else
                        elog(ERROR, "unexpected SET TRANSACTION element: %s",
                             item->defname);
                }
            }
            else if (strcmp(stmt->name, "SESSION CHARACTERISTICS") == 0)
            {
                ListCell   *head;

#ifdef XCP
                /* SET SESSION CHARACTERISTICS assumes "session" */
                stmt->is_local = false;
#endif

                foreach(head, stmt->args)
                {
                    DefElem    *item = (DefElem *) lfirst(head);

                    if (strcmp(item->defname, "transaction_isolation") == 0)
                        SetPGVariable("default_transaction_isolation",
                                      list_make1(item->arg), stmt->is_local);
                    else if (strcmp(item->defname, "transaction_read_only") == 0)
                        SetPGVariable("default_transaction_read_only",
                                      list_make1(item->arg), stmt->is_local);
                    else if (strcmp(item->defname, "transaction_deferrable") == 0)
                        SetPGVariable("default_transaction_deferrable",
                                      list_make1(item->arg), stmt->is_local);
                    else
                        elog(ERROR, "unexpected SET SESSION element: %s",
                             item->defname);
                }
            }
            else if (strcmp(stmt->name, "TRANSACTION SNAPSHOT") == 0)
            {
                A_Const    *con = linitial_node(A_Const, stmt->args);

                if (stmt->is_local)
                    ereport(ERROR,
                            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                             errmsg("SET LOCAL TRANSACTION SNAPSHOT is not implemented")));

                WarnNoTransactionChain(isTopLevel, "SET TRANSACTION");
                Assert(nodeTag(&con->val) == T_String);
                ImportSnapshot(strVal(&con->val));
            }
            else
                elog(ERROR, "unexpected SET MULTI element: %s",
                     stmt->name);
            break;
        case VAR_SET_DEFAULT:
            if (stmt->is_local)
                WarnNoTransactionChain(isTopLevel, "SET LOCAL");
            /* fall through */
        case VAR_RESET:
            if (strcmp(stmt->name, "transaction_isolation") == 0)
                WarnNoTransactionChain(isTopLevel, "RESET TRANSACTION");

            (void) set_config_option(stmt->name,
                                     NULL,
                                     (superuser() ? PGC_SUSET : PGC_USERSET),
                                     PGC_S_SESSION,
                                     action, true, 0, false);
            break;
        case VAR_RESET_ALL:
            ResetAllOptions();
            break;
    }
}

/*
 * Get the value to assign for a VariableSetStmt, or NULL if it's RESET.
 * The result is palloc'd.
 *
 * This is exported for use by actions such as ALTER ROLE SET.
 */
char *
ExtractSetVariableArgs(VariableSetStmt *stmt)
{
    switch (stmt->kind)
    {
        case VAR_SET_VALUE:
            return flatten_set_variable_args(stmt->name, stmt->args);
        case VAR_SET_CURRENT:
            return GetConfigOptionByName(stmt->name, NULL, false);
        default:
            return NULL;
    }
}

/*
 * SetPGVariable - SET command exported as an easily-C-callable function.
 *
 * This provides access to SET TO value, as well as SET TO DEFAULT (expressed
 * by passing args == NIL), but not SET FROM CURRENT functionality.
 */
void
SetPGVariable(const char *name, List *args, bool is_local)
{
    char       *argstring = flatten_set_variable_args(name, args);

    /* Note SET DEFAULT (argstring == NULL) is equivalent to RESET */
    (void) set_config_option(name,
                             argstring,
                             (superuser() ? PGC_SUSET : PGC_USERSET),
                             PGC_S_SESSION,
                             is_local ? GUC_ACTION_LOCAL : GUC_ACTION_SET,
                             true, 0, false);
}

/*
 * SET command wrapped as a SQL callable function.
 */
Datum
set_config_by_name(PG_FUNCTION_ARGS)
{
    char       *name;
    char       *value;
    char       *new_value;
    bool        is_local;

    if (PG_ARGISNULL(0))
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("SET requires parameter name")));

    /* Get the GUC variable name */
    name = TextDatumGetCString(PG_GETARG_DATUM(0));

    /* Get the desired value or set to NULL for a reset request */
    if (PG_ARGISNULL(1))
        value = NULL;
    else
        value = TextDatumGetCString(PG_GETARG_DATUM(1));

    /*
     * Get the desired state of is_local. Default to false if provided value
     * is NULL
     */
    if (PG_ARGISNULL(2))
        is_local = false;
    else
        is_local = PG_GETARG_BOOL(2);

    /* Note SET DEFAULT (argstring == NULL) is equivalent to RESET */
    (void) set_config_option(name,
                             value,
                             (superuser() ? PGC_SUSET : PGC_USERSET),
                             PGC_S_SESSION,
                             is_local ? GUC_ACTION_LOCAL : GUC_ACTION_SET,
                             true, 0, false);

    /* get the new current value */
    new_value = GetConfigOptionByName(name, NULL, false);

    /* Convert return string to text */
    PG_RETURN_TEXT_P(cstring_to_text(new_value));
}


/*
 * Common code for DefineCustomXXXVariable subroutines: allocate the
 * new variable's config struct and fill in generic fields.
 */
static struct config_generic *
init_custom_variable(const char *name,
                     const char *short_desc,
                     const char *long_desc,
                     GucContext context,
                     int flags,
                     enum config_type type,
                     size_t sz)
{
    struct config_generic *gen;

    /*
     * Only allow custom PGC_POSTMASTER variables to be created during shared
     * library preload; any later than that, we can't ensure that the value
     * doesn't change after startup.  This is a fatal elog if it happens; just
     * erroring out isn't safe because we don't know what the calling loadable
     * module might already have hooked into.
     */
    if (context == PGC_POSTMASTER &&
        !process_shared_preload_libraries_in_progress)
        elog(FATAL, "cannot create PGC_POSTMASTER variables after startup");

    /*
     * Before pljava commit 398f3b876ed402bdaec8bc804f29e2be95c75139
     * (2015-12-15), two of that module's PGC_USERSET variables facilitated
     * trivial escalation to superuser privileges.  Restrict the variables to
     * protect sites that have yet to upgrade pljava.
     */
    if (context == PGC_USERSET &&
        (strcmp(name, "pljava.classpath") == 0 ||
         strcmp(name, "pljava.vmoptions") == 0))
        context = PGC_SUSET;

    gen = (struct config_generic *) guc_malloc(ERROR, sz);
    memset(gen, 0, sz);

    gen->name = guc_strdup(ERROR, name);
    gen->context = context;
    gen->group = CUSTOM_OPTIONS;
    gen->short_desc = short_desc;
    gen->long_desc = long_desc;
    gen->flags = flags;
    gen->vartype = type;

    return gen;
}

/*
 * Common code for DefineCustomXXXVariable subroutines: insert the new
 * variable into the GUC variable array, replacing any placeholder.
 */
static void
define_custom_variable(struct config_generic *variable)
{
    const char *name = variable->name;
    const char **nameAddr = &name;
    struct config_string *pHolder;
    struct config_generic **res;

    /*
     * See if there's a placeholder by the same name.
     */
    res = (struct config_generic **) bsearch((void *) &nameAddr,
                                             (void *) guc_variables,
                                             num_guc_variables,
                                             sizeof(struct config_generic *),
                                             guc_var_compare);
    if (res == NULL)
    {
        /*
         * No placeholder to replace, so we can just add it ... but first,
         * make sure it's initialized to its default value.
         */
        InitializeOneGUCOption(variable);
        add_guc_variable(variable, ERROR);
        return;
    }

    /*
     * This better be a placeholder
     */
    if (((*res)->flags & GUC_CUSTOM_PLACEHOLDER) == 0)
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("attempt to redefine parameter \"%s\"", name)));

    Assert((*res)->vartype == PGC_STRING);
    pHolder = (struct config_string *) (*res);

    /*
     * First, set the variable to its default value.  We must do this even
     * though we intend to immediately apply a new value, since it's possible
     * that the new value is invalid.
     */
    InitializeOneGUCOption(variable);

    /*
     * Replace the placeholder. We aren't changing the name, so no re-sorting
     * is necessary
     */
    *res = variable;

    /*
     * Assign the string value(s) stored in the placeholder to the real
     * variable.  Essentially, we need to duplicate all the active and stacked
     * values, but with appropriate validation and datatype adjustment.
     *
     * If an assignment fails, we report a WARNING and keep going.  We don't
     * want to throw ERROR for bad values, because it'd bollix the add-on
     * module that's presumably halfway through getting loaded.  In such cases
     * the default or previous state will become active instead.
     */

    /* First, apply the reset value if any */
    if (pHolder->reset_val)
        (void) set_config_option(name, pHolder->reset_val,
                                 pHolder->gen.reset_scontext,
                                 pHolder->gen.reset_source,
                                 GUC_ACTION_SET, true, WARNING, false);
    /* That should not have resulted in stacking anything */
    Assert(variable->stack == NULL);

    /* Now, apply current and stacked values, in the order they were stacked */
    reapply_stacked_values(variable, pHolder, pHolder->gen.stack,
                           *(pHolder->variable),
                           pHolder->gen.scontext, pHolder->gen.source);

    /* Also copy over any saved source-location information */
    if (pHolder->gen.sourcefile)
        set_config_sourcefile(name, pHolder->gen.sourcefile,
                              pHolder->gen.sourceline);

    /*
     * Free up as much as we conveniently can of the placeholder structure.
     * (This neglects any stack items, so it's possible for some memory to be
     * leaked.  Since this can only happen once per session per variable, it
     * doesn't seem worth spending much code on.)
     */
    set_string_field(pHolder, pHolder->variable, NULL);
    set_string_field(pHolder, &pHolder->reset_val, NULL);

    free(pHolder);
}

/*
 * Recursive subroutine for define_custom_variable: reapply non-reset values
 *
 * We recurse so that the values are applied in the same order as originally.
 * At each recursion level, apply the upper-level value (passed in) in the
 * fashion implied by the stack entry.
 */
static void
reapply_stacked_values(struct config_generic *variable,
                       struct config_string *pHolder,
                       GucStack *stack,
                       const char *curvalue,
                       GucContext curscontext, GucSource cursource)
{// #lizard forgives
    const char *name = variable->name;
    GucStack   *oldvarstack = variable->stack;

    if (stack != NULL)
    {
        /* First, recurse, so that stack items are processed bottom to top */
        reapply_stacked_values(variable, pHolder, stack->prev,
                               stack->prior.val.stringval,
                               stack->scontext, stack->source);

        /* See how to apply the passed-in value */
        switch (stack->state)
        {
            case GUC_SAVE:
                (void) set_config_option(name, curvalue,
                                         curscontext, cursource,
                                         GUC_ACTION_SAVE, true,
                                         WARNING, false);
                break;

            case GUC_SET:
                (void) set_config_option(name, curvalue,
                                         curscontext, cursource,
                                         GUC_ACTION_SET, true,
                                         WARNING, false);
                break;

            case GUC_LOCAL:
                (void) set_config_option(name, curvalue,
                                         curscontext, cursource,
                                         GUC_ACTION_LOCAL, true,
                                         WARNING, false);
                break;

            case GUC_SET_LOCAL:
                /* first, apply the masked value as SET */
                (void) set_config_option(name, stack->masked.val.stringval,
                                         stack->masked_scontext, PGC_S_SESSION,
                                         GUC_ACTION_SET, true,
                                         WARNING, false);
                /* then apply the current value as LOCAL */
                (void) set_config_option(name, curvalue,
                                         curscontext, cursource,
                                         GUC_ACTION_LOCAL, true,
                                         WARNING, false);
                break;
        }

        /* If we successfully made a stack entry, adjust its nest level */
        if (variable->stack != oldvarstack)
            variable->stack->nest_level = stack->nest_level;
    }
    else
    {
        /*
         * We are at the end of the stack.  If the active/previous value is
         * different from the reset value, it must represent a previously
         * committed session value.  Apply it, and then drop the stack entry
         * that set_config_option will have created under the impression that
         * this is to be just a transactional assignment.  (We leak the stack
         * entry.)
         */
        if (curvalue != pHolder->reset_val ||
            curscontext != pHolder->gen.reset_scontext ||
            cursource != pHolder->gen.reset_source)
        {
            (void) set_config_option(name, curvalue,
                                     curscontext, cursource,
                                     GUC_ACTION_SET, true, WARNING, false);
            variable->stack = NULL;
        }
    }
}

void
DefineCustomBoolVariable(const char *name,
                         const char *short_desc,
                         const char *long_desc,
                         bool *valueAddr,
                         bool bootValue,
                         GucContext context,
                         int flags,
                         GucBoolCheckHook check_hook,
                         GucBoolAssignHook assign_hook,
                         GucShowHook show_hook)
{
    struct config_bool *var;

    var = (struct config_bool *)
        init_custom_variable(name, short_desc, long_desc, context, flags,
                             PGC_BOOL, sizeof(struct config_bool));
    var->variable = valueAddr;
    var->boot_val = bootValue;
    var->reset_val = bootValue;
    var->check_hook = check_hook;
    var->assign_hook = assign_hook;
    var->show_hook = show_hook;
    define_custom_variable(&var->gen);
}

void
DefineCustomIntVariable(const char *name,
                        const char *short_desc,
                        const char *long_desc,
                        int *valueAddr,
                        int bootValue,
                        int minValue,
                        int maxValue,
                        GucContext context,
                        int flags,
                        GucIntCheckHook check_hook,
                        GucIntAssignHook assign_hook,
                        GucShowHook show_hook)
{
    struct config_int *var;

    var = (struct config_int *)
        init_custom_variable(name, short_desc, long_desc, context, flags,
                             PGC_INT, sizeof(struct config_int));
    var->variable = valueAddr;
    var->boot_val = bootValue;
    var->reset_val = bootValue;
    var->min = minValue;
    var->max = maxValue;
    var->check_hook = check_hook;
    var->assign_hook = assign_hook;
    var->show_hook = show_hook;
    define_custom_variable(&var->gen);
}

void
DefineCustomUintVariable(const char *name,
                         const char *short_desc,
                         const char *long_desc,
                         uint *valueAddr,
                         uint bootValue,
                         uint minValue,
                         uint maxValue,
                         GucContext context,
                         int flags,
                         GucUintCheckHook check_hook,
                         GucUintAssignHook assign_hook,
                         GucShowHook show_hook)
{
    struct config_uint *var;

    var = (struct config_uint *)
        init_custom_variable(name, short_desc, long_desc, context, flags,
                             PGC_UINT, sizeof(struct config_uint));
    var->variable = valueAddr;
    var->boot_val = bootValue;
    var->reset_val = bootValue;
    var->min = minValue;
    var->max = maxValue;
    var->check_hook = check_hook;
    var->assign_hook = assign_hook;
    var->show_hook = show_hook;
    define_custom_variable(&var->gen);
}

void
DefineCustomRealVariable(const char *name,
                         const char *short_desc,
                         const char *long_desc,
                         double *valueAddr,
                         double bootValue,
                         double minValue,
                         double maxValue,
                         GucContext context,
                         int flags,
                         GucRealCheckHook check_hook,
                         GucRealAssignHook assign_hook,
                         GucShowHook show_hook)
{
    struct config_real *var;

    var = (struct config_real *)
        init_custom_variable(name, short_desc, long_desc, context, flags,
                             PGC_REAL, sizeof(struct config_real));
    var->variable = valueAddr;
    var->boot_val = bootValue;
    var->reset_val = bootValue;
    var->min = minValue;
    var->max = maxValue;
    var->check_hook = check_hook;
    var->assign_hook = assign_hook;
    var->show_hook = show_hook;
    define_custom_variable(&var->gen);
}

void
DefineCustomStringVariable(const char *name,
                           const char *short_desc,
                           const char *long_desc,
                           char **valueAddr,
                           const char *bootValue,
                           GucContext context,
                           int flags,
                           GucStringCheckHook check_hook,
                           GucStringAssignHook assign_hook,
                           GucShowHook show_hook)
{
    struct config_string *var;

    var = (struct config_string *)
        init_custom_variable(name, short_desc, long_desc, context, flags,
                             PGC_STRING, sizeof(struct config_string));
    var->variable = valueAddr;
    var->boot_val = bootValue;
    var->check_hook = check_hook;
    var->assign_hook = assign_hook;
    var->show_hook = show_hook;
    define_custom_variable(&var->gen);
}

void
DefineCustomEnumVariable(const char *name,
                         const char *short_desc,
                         const char *long_desc,
                         int *valueAddr,
                         int bootValue,
                         const struct config_enum_entry *options,
                         GucContext context,
                         int flags,
                         GucEnumCheckHook check_hook,
                         GucEnumAssignHook assign_hook,
                         GucShowHook show_hook)
{
    struct config_enum *var;

    var = (struct config_enum *)
        init_custom_variable(name, short_desc, long_desc, context, flags,
                             PGC_ENUM, sizeof(struct config_enum));
    var->variable = valueAddr;
    var->boot_val = bootValue;
    var->reset_val = bootValue;
    var->options = options;
    var->check_hook = check_hook;
    var->assign_hook = assign_hook;
    var->show_hook = show_hook;
    define_custom_variable(&var->gen);
}

void
EmitWarningsOnPlaceholders(const char *className)
{
    int            classLen = strlen(className);
    int            i;

    for (i = 0; i < num_guc_variables; i++)
    {
        struct config_generic *var = guc_variables[i];

        if ((var->flags & GUC_CUSTOM_PLACEHOLDER) != 0 &&
            strncmp(className, var->name, classLen) == 0 &&
            var->name[classLen] == GUC_QUALIFIER_SEPARATOR)
        {
            ereport(WARNING,
                    (errcode(ERRCODE_UNDEFINED_OBJECT),
                     errmsg("unrecognized configuration parameter \"%s\"",
                            var->name)));
        }
    }
}


/*
 * SHOW command
 */
void
GetPGVariable(const char *name, DestReceiver *dest)
{
    if (guc_name_compare(name, "all") == 0)
        ShowAllGUCConfig(dest);
    else
        ShowGUCConfigOption(name, dest);
}

TupleDesc
GetPGVariableResultDesc(const char *name)
{
    TupleDesc    tupdesc;

    if (guc_name_compare(name, "all") == 0)
    {
        /* need a tuple descriptor representing three TEXT columns */
        tupdesc = CreateTemplateTupleDesc(3, false);
        TupleDescInitEntry(tupdesc, (AttrNumber) 1, "name",
                           TEXTOID, -1, 0);
        TupleDescInitEntry(tupdesc, (AttrNumber) 2, "setting",
                           TEXTOID, -1, 0);
        TupleDescInitEntry(tupdesc, (AttrNumber) 3, "description",
                           TEXTOID, -1, 0);
    }
    else
    {
        const char *varname;

        /* Get the canonical spelling of name */
        (void) GetConfigOptionByName(name, &varname, false);

        /* need a tuple descriptor representing a single TEXT column */
        tupdesc = CreateTemplateTupleDesc(1, false);
        TupleDescInitEntry(tupdesc, (AttrNumber) 1, varname,
                           TEXTOID, -1, 0);
    }
    return tupdesc;
}


/*
 * SHOW command
 */
static void
ShowGUCConfigOption(const char *name, DestReceiver *dest)
{
    TupOutputState *tstate;
    TupleDesc    tupdesc;
    const char *varname;
    char       *value;

    /* Get the value and canonical spelling of name */
    value = GetConfigOptionByName(name, &varname, false);

    /* need a tuple descriptor representing a single TEXT column */
    tupdesc = CreateTemplateTupleDesc(1, false);
    TupleDescInitBuiltinEntry(tupdesc, (AttrNumber) 1, varname,
                              TEXTOID, -1, 0);

    /* prepare for projection of tuples */
    tstate = begin_tup_output_tupdesc(dest, tupdesc);

    /* Send it */
    do_text_output_oneline(tstate, value);

    end_tup_output(tstate);
}

/*
 * SHOW ALL command
 */
static void
ShowAllGUCConfig(DestReceiver *dest)
{
    bool        am_superuser = superuser();
    int            i;
    TupOutputState *tstate;
    TupleDesc    tupdesc;
    Datum        values[3];
    bool        isnull[3] = {false, false, false};

    /* need a tuple descriptor representing three TEXT columns */
    tupdesc = CreateTemplateTupleDesc(3, false);
    TupleDescInitBuiltinEntry(tupdesc, (AttrNumber) 1, "name",
                              TEXTOID, -1, 0);
    TupleDescInitBuiltinEntry(tupdesc, (AttrNumber) 2, "setting",
                              TEXTOID, -1, 0);
    TupleDescInitBuiltinEntry(tupdesc, (AttrNumber) 3, "description",
                              TEXTOID, -1, 0);

    /* prepare for projection of tuples */
    tstate = begin_tup_output_tupdesc(dest, tupdesc);

    for (i = 0; i < num_guc_variables; i++)
    {
        struct config_generic *conf = guc_variables[i];
        char       *setting;

        if ((conf->flags & GUC_NO_SHOW_ALL) ||
            ((conf->flags & GUC_SUPERUSER_ONLY) && !am_superuser))
            continue;

        /* assign to the values array */
        values[0] = PointerGetDatum(cstring_to_text(conf->name));

        setting = _ShowOption(conf, true);
        if (setting)
        {
            values[1] = PointerGetDatum(cstring_to_text(setting));
            isnull[1] = false;
        }
        else
        {
            values[1] = PointerGetDatum(NULL);
            isnull[1] = true;
        }

        values[2] = PointerGetDatum(cstring_to_text(conf->short_desc));

        /* send it to dest */
        do_tup_output(tstate, values, isnull);

        /* clean up */
        pfree(DatumGetPointer(values[0]));
        if (setting)
        {
            pfree(setting);
            pfree(DatumGetPointer(values[1]));
        }
        pfree(DatumGetPointer(values[2]));
    }

    end_tup_output(tstate);
}

/*
 * Return GUC variable value by name; optionally return canonical form of
 * name.  If the GUC is unset, then throw an error unless missing_ok is true,
 * in which case return NULL.  Return value is palloc'd (but *varname isn't).
 */
char *
GetConfigOptionByName(const char *name, const char **varname, bool missing_ok)
{
    struct config_generic *record;

    record = find_option(name, false, ERROR);
    if (record == NULL)
    {
        if (missing_ok)
        {
            if (varname)
                *varname = NULL;
            return NULL;
        }

        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_OBJECT),
                 errmsg("unrecognized configuration parameter \"%s\"", name)));
    }

    if ((record->flags & GUC_SUPERUSER_ONLY) &&
        !is_member_of_role(GetUserId(), DEFAULT_ROLE_READ_ALL_SETTINGS))
        ereport(ERROR,
                (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                 errmsg("must be superuser or a member of pg_read_all_settings to examine \"%s\"",
                        name)));

    if (varname)
        *varname = record->name;

    return _ShowOption(record, true);
}

/*
 * Return GUC variable value by variable number; optionally return canonical
 * form of name.  Return value is palloc'd.
 */
void
GetConfigOptionByNum(int varnum, const char **values, bool *noshow)
{// #lizard forgives
    char        buffer[256];
    struct config_generic *conf;

    /* check requested variable number valid */
    Assert((varnum >= 0) && (varnum < num_guc_variables));

    conf = guc_variables[varnum];

    if (noshow)
    {
        if ((conf->flags & GUC_NO_SHOW_ALL) ||
            ((conf->flags & GUC_SUPERUSER_ONLY) &&
             !is_member_of_role(GetUserId(), DEFAULT_ROLE_READ_ALL_SETTINGS)))
            *noshow = true;
        else
            *noshow = false;
    }

    /* first get the generic attributes */

    /* name */
    values[0] = conf->name;

    /* setting : use _ShowOption in order to avoid duplicating the logic */
    values[1] = _ShowOption(conf, false);

    /* unit */
    if (conf->vartype == PGC_INT)
    {
        switch (conf->flags & (GUC_UNIT_MEMORY | GUC_UNIT_TIME))
        {
            case GUC_UNIT_KB:
                values[2] = "kB";
                break;
            case GUC_UNIT_MB:
                values[2] = "MB";
                break;
            case GUC_UNIT_BLOCKS:
                snprintf(buffer, sizeof(buffer), "%dkB", BLCKSZ / 1024);
                values[2] = pstrdup(buffer);
                break;
            case GUC_UNIT_XBLOCKS:
                snprintf(buffer, sizeof(buffer), "%dkB", XLOG_BLCKSZ / 1024);
                values[2] = pstrdup(buffer);
                break;
            case GUC_UNIT_MS:
                values[2] = "ms";
                break;
            case GUC_UNIT_S:
                values[2] = "s";
                break;
            case GUC_UNIT_MIN:
                values[2] = "min";
                break;
            case 0:
                values[2] = NULL;
                break;
            default:
                elog(ERROR, "unrecognized GUC units value: %d",
                     conf->flags & (GUC_UNIT_MEMORY | GUC_UNIT_TIME));
                values[2] = NULL;
                break;
        }
    }
    else
        values[2] = NULL;

    /* group */
    values[3] = config_group_names[conf->group];

    /* short_desc */
    values[4] = conf->short_desc;

    /* extra_desc */
    values[5] = conf->long_desc;

    /* context */
    values[6] = GucContext_Names[conf->context];

    /* vartype */
    values[7] = config_type_names[conf->vartype];

    /* source */
    values[8] = GucSource_Names[conf->source];

    /* now get the type specific attributes */
    switch (conf->vartype)
    {
        case PGC_BOOL:
            {
                struct config_bool *lconf = (struct config_bool *) conf;

                /* min_val */
                values[9] = NULL;

                /* max_val */
                values[10] = NULL;

                /* enumvals */
                values[11] = NULL;

                /* boot_val */
                values[12] = pstrdup(lconf->boot_val ? "on" : "off");

                /* reset_val */
                values[13] = pstrdup(lconf->reset_val ? "on" : "off");
            }
            break;

        case PGC_INT:
            {
                struct config_int *lconf = (struct config_int *) conf;

                /* min_val */
                snprintf(buffer, sizeof(buffer), "%d", lconf->min);
                values[9] = pstrdup(buffer);

                /* max_val */
                snprintf(buffer, sizeof(buffer), "%d", lconf->max);
                values[10] = pstrdup(buffer);

                /* enumvals */
                values[11] = NULL;

                /* boot_val */
                snprintf(buffer, sizeof(buffer), "%d", lconf->boot_val);
                values[12] = pstrdup(buffer);

                /* reset_val */
                snprintf(buffer, sizeof(buffer), "%d", lconf->reset_val);
                values[13] = pstrdup(buffer);
            }
            break;

        case PGC_UINT:
            {
                struct config_uint *lconf = (struct config_uint *) conf;

                /* min_val */
                snprintf(buffer, sizeof(buffer), "%u", lconf->min);
                values[9] = pstrdup(buffer);

                /* max_val */
                snprintf(buffer, sizeof(buffer), "%u", lconf->max);
                values[10] = pstrdup(buffer);

                /* enumvals */
                values[11] = NULL;

                /* boot_val */
                snprintf(buffer, sizeof(buffer), "%u", lconf->boot_val);
                values[12] = pstrdup(buffer);

                /* reset_val */
                snprintf(buffer, sizeof(buffer), "%u", lconf->reset_val);
                values[13] = pstrdup(buffer);
            }
            break;

        case PGC_REAL:
            {
                struct config_real *lconf = (struct config_real *) conf;

                /* min_val */
                snprintf(buffer, sizeof(buffer), "%g", lconf->min);
                values[9] = pstrdup(buffer);

                /* max_val */
                snprintf(buffer, sizeof(buffer), "%g", lconf->max);
                values[10] = pstrdup(buffer);

                /* enumvals */
                values[11] = NULL;

                /* boot_val */
                snprintf(buffer, sizeof(buffer), "%g", lconf->boot_val);
                values[12] = pstrdup(buffer);

                /* reset_val */
                snprintf(buffer, sizeof(buffer), "%g", lconf->reset_val);
                values[13] = pstrdup(buffer);
            }
            break;

        case PGC_STRING:
            {
                struct config_string *lconf = (struct config_string *) conf;

                /* min_val */
                values[9] = NULL;

                /* max_val */
                values[10] = NULL;

                /* enumvals */
                values[11] = NULL;

                /* boot_val */
                if (lconf->boot_val == NULL)
                    values[12] = NULL;
                else
                    values[12] = pstrdup(lconf->boot_val);

                /* reset_val */
                if (lconf->reset_val == NULL)
                    values[13] = NULL;
                else
                    values[13] = pstrdup(lconf->reset_val);
            }
            break;

        case PGC_ENUM:
            {
                struct config_enum *lconf = (struct config_enum *) conf;

                /* min_val */
                values[9] = NULL;

                /* max_val */
                values[10] = NULL;

                /* enumvals */

                /*
                 * NOTE! enumvals with double quotes in them are not
                 * supported!
                 */
                values[11] = config_enum_get_options((struct config_enum *) conf,
                                                     "{\"", "\"}", "\",\"");

                /* boot_val */
                values[12] = pstrdup(config_enum_lookup_by_value(lconf,
                                                                 lconf->boot_val));

                /* reset_val */
                values[13] = pstrdup(config_enum_lookup_by_value(lconf,
                                                                 lconf->reset_val));
            }
            break;

        default:
            {
                /*
                 * should never get here, but in case we do, set 'em to NULL
                 */

                /* min_val */
                values[9] = NULL;

                /* max_val */
                values[10] = NULL;

                /* enumvals */
                values[11] = NULL;

                /* boot_val */
                values[12] = NULL;

                /* reset_val */
                values[13] = NULL;
            }
            break;
    }

    /*
     * If the setting came from a config file, set the source location. For
     * security reasons, we don't show source file/line number for
     * non-superusers.
     */
    if (conf->source == PGC_S_FILE && superuser())
    {
        values[14] = conf->sourcefile;
        snprintf(buffer, sizeof(buffer), "%d", conf->sourceline);
        values[15] = pstrdup(buffer);
    }
    else
    {
        values[14] = NULL;
        values[15] = NULL;
    }

    values[16] = (conf->status & GUC_PENDING_RESTART) ? "t" : "f";
}

/*
 * Return the total number of GUC variables
 */
int
GetNumConfigOptions(void)
{
    return num_guc_variables;
}

/*
 * show_config_by_name - equiv to SHOW X command but implemented as
 * a function.
 */
Datum
show_config_by_name(PG_FUNCTION_ARGS)
{
    char       *varname = TextDatumGetCString(PG_GETARG_DATUM(0));
    char       *varval;

    /* Get the value */
    varval = GetConfigOptionByName(varname, NULL, false);

    /* Convert to text */
    PG_RETURN_TEXT_P(cstring_to_text(varval));
}

/*
 * show_config_by_name_missing_ok - equiv to SHOW X command but implemented as
 * a function.  If X does not exist, suppress the error and just return NULL
 * if missing_ok is TRUE.
 */
Datum
show_config_by_name_missing_ok(PG_FUNCTION_ARGS)
{
    char       *varname = TextDatumGetCString(PG_GETARG_DATUM(0));
    bool        missing_ok = PG_GETARG_BOOL(1);
    char       *varval;

    /* Get the value */
    varval = GetConfigOptionByName(varname, NULL, missing_ok);

    /* return NULL if no such variable */
    if (varval == NULL)
        PG_RETURN_NULL();

    /* Convert to text */
    PG_RETURN_TEXT_P(cstring_to_text(varval));
}

/*
 * show_all_settings - equiv to SHOW ALL command but implemented as
 * a Table Function.
 */
#define NUM_PG_SETTINGS_ATTS    17

Datum
show_all_settings(PG_FUNCTION_ARGS)
{
    FuncCallContext *funcctx;
    TupleDesc    tupdesc;
    int            call_cntr;
    int            max_calls;
    AttInMetadata *attinmeta;
    MemoryContext oldcontext;

    /* stuff done only on the first call of the function */
    if (SRF_IS_FIRSTCALL())
    {
        /* create a function context for cross-call persistence */
        funcctx = SRF_FIRSTCALL_INIT();

        /*
         * switch to memory context appropriate for multiple function calls
         */
        oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

        /*
         * need a tuple descriptor representing NUM_PG_SETTINGS_ATTS columns
         * of the appropriate types
         */
        tupdesc = CreateTemplateTupleDesc(NUM_PG_SETTINGS_ATTS, false);
        TupleDescInitEntry(tupdesc, (AttrNumber) 1, "name",
                           TEXTOID, -1, 0);
        TupleDescInitEntry(tupdesc, (AttrNumber) 2, "setting",
                           TEXTOID, -1, 0);
        TupleDescInitEntry(tupdesc, (AttrNumber) 3, "unit",
                           TEXTOID, -1, 0);
        TupleDescInitEntry(tupdesc, (AttrNumber) 4, "category",
                           TEXTOID, -1, 0);
        TupleDescInitEntry(tupdesc, (AttrNumber) 5, "short_desc",
                           TEXTOID, -1, 0);
        TupleDescInitEntry(tupdesc, (AttrNumber) 6, "extra_desc",
                           TEXTOID, -1, 0);
        TupleDescInitEntry(tupdesc, (AttrNumber) 7, "context",
                           TEXTOID, -1, 0);
        TupleDescInitEntry(tupdesc, (AttrNumber) 8, "vartype",
                           TEXTOID, -1, 0);
        TupleDescInitEntry(tupdesc, (AttrNumber) 9, "source",
                           TEXTOID, -1, 0);
        TupleDescInitEntry(tupdesc, (AttrNumber) 10, "min_val",
                           TEXTOID, -1, 0);
        TupleDescInitEntry(tupdesc, (AttrNumber) 11, "max_val",
                           TEXTOID, -1, 0);
        TupleDescInitEntry(tupdesc, (AttrNumber) 12, "enumvals",
                           TEXTARRAYOID, -1, 0);
        TupleDescInitEntry(tupdesc, (AttrNumber) 13, "boot_val",
                           TEXTOID, -1, 0);
        TupleDescInitEntry(tupdesc, (AttrNumber) 14, "reset_val",
                           TEXTOID, -1, 0);
        TupleDescInitEntry(tupdesc, (AttrNumber) 15, "sourcefile",
                           TEXTOID, -1, 0);
        TupleDescInitEntry(tupdesc, (AttrNumber) 16, "sourceline",
                           INT4OID, -1, 0);
        TupleDescInitEntry(tupdesc, (AttrNumber) 17, "pending_restart",
                           BOOLOID, -1, 0);

        /*
         * Generate attribute metadata needed later to produce tuples from raw
         * C strings
         */
        attinmeta = TupleDescGetAttInMetadata(tupdesc);
        funcctx->attinmeta = attinmeta;

        /* total number of tuples to be returned */
        funcctx->max_calls = GetNumConfigOptions();

        MemoryContextSwitchTo(oldcontext);
    }

    /* stuff done on every call of the function */
    funcctx = SRF_PERCALL_SETUP();

    call_cntr = funcctx->call_cntr;
    max_calls = funcctx->max_calls;
    attinmeta = funcctx->attinmeta;

    if (call_cntr < max_calls)    /* do when there is more left to send */
    {
        char       *values[NUM_PG_SETTINGS_ATTS];
        bool        noshow;
        HeapTuple    tuple;
        Datum        result;

        /*
         * Get the next visible GUC variable name and value
         */
        do
        {
            GetConfigOptionByNum(call_cntr, (const char **) values, &noshow);
            if (noshow)
            {
                /* bump the counter and get the next config setting */
                call_cntr = ++funcctx->call_cntr;

                /* make sure we haven't gone too far now */
                if (call_cntr >= max_calls)
                    SRF_RETURN_DONE(funcctx);
            }
        } while (noshow);

        /* build a tuple */
        tuple = BuildTupleFromCStrings(attinmeta, values);

        /* make the tuple into a datum */
        result = HeapTupleGetDatum(tuple);

        SRF_RETURN_NEXT(funcctx, result);
    }
    else
    {
        /* do when there is no more left */
        SRF_RETURN_DONE(funcctx);
    }
}

/*
 * show_all_file_settings
 *
 * Returns a table of all parameter settings in all configuration files
 * which includes the config file pathname, the line number, a sequence number
 * indicating the order in which the settings were encountered, the parameter
 * name and value, a bool showing if the value could be applied, and possibly
 * an associated error message.  (For problems such as syntax errors, the
 * parameter name/value might be NULL.)
 *
 * Note: no filtering is done here, instead we depend on the GRANT system
 * to prevent unprivileged users from accessing this function or the view
 * built on top of it.
 */
Datum
show_all_file_settings(PG_FUNCTION_ARGS)
{// #lizard forgives
#define NUM_PG_FILE_SETTINGS_ATTS 7
    ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
    TupleDesc    tupdesc;
    Tuplestorestate *tupstore;
    ConfigVariable *conf;
    int            seqno;
    MemoryContext per_query_ctx;
    MemoryContext oldcontext;

    /* Check to see if caller supports us returning a tuplestore */
    if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("set-valued function called in context that cannot accept a set")));
    if (!(rsinfo->allowedModes & SFRM_Materialize))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("materialize mode required, but it is not " \
                        "allowed in this context")));

    /* Scan the config files using current context as workspace */
    conf = ProcessConfigFileInternal(PGC_SIGHUP, false, DEBUG3);

    /* Switch into long-lived context to construct returned data structures */
    per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
    oldcontext = MemoryContextSwitchTo(per_query_ctx);

    /* Build a tuple descriptor for our result type */
    tupdesc = CreateTemplateTupleDesc(NUM_PG_FILE_SETTINGS_ATTS, false);
    TupleDescInitEntry(tupdesc, (AttrNumber) 1, "sourcefile",
                       TEXTOID, -1, 0);
    TupleDescInitEntry(tupdesc, (AttrNumber) 2, "sourceline",
                       INT4OID, -1, 0);
    TupleDescInitEntry(tupdesc, (AttrNumber) 3, "seqno",
                       INT4OID, -1, 0);
    TupleDescInitEntry(tupdesc, (AttrNumber) 4, "name",
                       TEXTOID, -1, 0);
    TupleDescInitEntry(tupdesc, (AttrNumber) 5, "setting",
                       TEXTOID, -1, 0);
    TupleDescInitEntry(tupdesc, (AttrNumber) 6, "applied",
                       BOOLOID, -1, 0);
    TupleDescInitEntry(tupdesc, (AttrNumber) 7, "error",
                       TEXTOID, -1, 0);

    /* Build a tuplestore to return our results in */
    tupstore = tuplestore_begin_heap(true, false, work_mem);
    rsinfo->returnMode = SFRM_Materialize;
    rsinfo->setResult = tupstore;
    rsinfo->setDesc = tupdesc;

    /* The rest can be done in short-lived context */
    MemoryContextSwitchTo(oldcontext);

    /* Process the results and create a tuplestore */
    for (seqno = 1; conf != NULL; conf = conf->next, seqno++)
    {
        Datum        values[NUM_PG_FILE_SETTINGS_ATTS];
        bool        nulls[NUM_PG_FILE_SETTINGS_ATTS];

        memset(values, 0, sizeof(values));
        memset(nulls, 0, sizeof(nulls));

        /* sourcefile */
        if (conf->filename)
            values[0] = PointerGetDatum(cstring_to_text(conf->filename));
        else
            nulls[0] = true;

        /* sourceline (not meaningful if no sourcefile) */
        if (conf->filename)
            values[1] = Int32GetDatum(conf->sourceline);
        else
            nulls[1] = true;

        /* seqno */
        values[2] = Int32GetDatum(seqno);

        /* name */
        if (conf->name)
            values[3] = PointerGetDatum(cstring_to_text(conf->name));
        else
            nulls[3] = true;

        /* setting */
        if (conf->value)
            values[4] = PointerGetDatum(cstring_to_text(conf->value));
        else
            nulls[4] = true;

        /* applied */
        values[5] = BoolGetDatum(conf->applied);

        /* error */
        if (conf->errmsg)
            values[6] = PointerGetDatum(cstring_to_text(conf->errmsg));
        else
            nulls[6] = true;

        /* shove row into tuplestore */
        tuplestore_putvalues(tupstore, tupdesc, values, nulls);
    }

    tuplestore_donestoring(tupstore);

    return (Datum) 0;
}

static char *
_ShowOption(struct config_generic *record, bool use_units)
{// #lizard forgives
    char        buffer[256];
    const char *val;

    switch (record->vartype)
    {
        case PGC_BOOL:
            {
                struct config_bool *conf = (struct config_bool *) record;

                if (conf->show_hook)
                    val = (*conf->show_hook) ();
                else
                    val = *conf->variable ? "on" : "off";
            }
            break;

        case PGC_INT:
            {
                struct config_int *conf = (struct config_int *) record;

                if (conf->show_hook)
                    val = (*conf->show_hook) ();
                else
                {
                    /*
                     * Use int64 arithmetic to avoid overflows in units
                     * conversion.
                     */
                    int64        result = *conf->variable;
                    const char *unit;

                    if (use_units && result > 0 && (record->flags & GUC_UNIT))
                    {
                        convert_from_base_unit(result, record->flags & GUC_UNIT,
                                               &result, &unit);
                    }
                    else
                        unit = "";

                    snprintf(buffer, sizeof(buffer), INT64_FORMAT "%s",
                             result, unit);
                    val = buffer;
                }
            }
            break;

        case PGC_UINT:
            {
                struct config_uint *conf = (struct config_uint *) record;

                if (conf->show_hook)
                    val = (*conf->show_hook) ();
                else
                {
                    /*
                     * Use int64 arithmetic to avoid overflows in units
                     * conversion.
                     */
                    int64        result = *conf->variable;
                    const char *unit;

                    if (use_units && result > 0 && (record->flags & GUC_UNIT))
                    {
                        convert_from_base_unit(result, record->flags & GUC_UNIT,
                                               &result, &unit);
                    }
                    else
                        unit = "";

                    snprintf(buffer, sizeof(buffer), UINT64_FORMAT "%s",
                             result, unit);
                    val = buffer;
                }
            }
            break;

        case PGC_REAL:
            {
                struct config_real *conf = (struct config_real *) record;

                if (conf->show_hook)
                    val = (*conf->show_hook) ();
                else
                {
                    snprintf(buffer, sizeof(buffer), "%g",
                             *conf->variable);
                    val = buffer;
                }
            }
            break;

        case PGC_STRING:
            {
                struct config_string *conf = (struct config_string *) record;

                if (conf->show_hook)
                    val = (*conf->show_hook) ();
                else if (*conf->variable && **conf->variable)
                    val = *conf->variable;
                else
                    val = "";
            }
            break;

        case PGC_ENUM:
            {
                struct config_enum *conf = (struct config_enum *) record;

                if (conf->show_hook)
                    val = (*conf->show_hook) ();
                else
                    val = config_enum_lookup_by_value(conf, *conf->variable);
            }
            break;

        default:
            /* just to keep compiler quiet */
            val = "???";
            break;
    }

    return pstrdup(val);
}


#ifdef EXEC_BACKEND

/*
 *    These routines dump out all non-default GUC options into a binary
 *    file that is read by all exec'ed backends.  The format is:
 *
 *        variable name, string, null terminated
 *        variable value, string, null terminated
 *        variable sourcefile, string, null terminated (empty if none)
 *        variable sourceline, integer
 *        variable source, integer
 *        variable scontext, integer
 */
static void
write_one_nondefault_variable(FILE *fp, struct config_generic *gconf)
{// #lizard forgives
    if (gconf->source == PGC_S_DEFAULT)
        return;

    fprintf(fp, "%s", gconf->name);
    fputc(0, fp);

    switch (gconf->vartype)
    {
        case PGC_BOOL:
            {
                struct config_bool *conf = (struct config_bool *) gconf;

                if (*conf->variable)
                    fprintf(fp, "true");
                else
                    fprintf(fp, "false");
            }
            break;

        case PGC_INT:
            {
                struct config_int *conf = (struct config_int *) gconf;

                fprintf(fp, "%d", *conf->variable);
            }
            break;

        case PGC_UINT:
            {
                struct config_uint *conf = (struct config_uint *) gconf;

                fprintf(fp, "%u", *conf->variable);
            }
            break;

        case PGC_REAL:
            {
                struct config_real *conf = (struct config_real *) gconf;

                fprintf(fp, "%.17g", *conf->variable);
            }
            break;

        case PGC_STRING:
            {
                struct config_string *conf = (struct config_string *) gconf;

                fprintf(fp, "%s", *conf->variable);
            }
            break;

        case PGC_ENUM:
            {
                struct config_enum *conf = (struct config_enum *) gconf;

                fprintf(fp, "%s",
                        config_enum_lookup_by_value(conf, *conf->variable));
            }
            break;
    }

    fputc(0, fp);

    if (gconf->sourcefile)
        fprintf(fp, "%s", gconf->sourcefile);
    fputc(0, fp);

    fwrite(&gconf->sourceline, 1, sizeof(gconf->sourceline), fp);
    fwrite(&gconf->source, 1, sizeof(gconf->source), fp);
    fwrite(&gconf->scontext, 1, sizeof(gconf->scontext), fp);
}

void
write_nondefault_variables(GucContext context)
{
    int            elevel;
    FILE       *fp;
    int            i;

    Assert(context == PGC_POSTMASTER || context == PGC_SIGHUP);

    elevel = (context == PGC_SIGHUP) ? LOG : ERROR;

    /*
     * Open file
     */
    fp = AllocateFile(CONFIG_EXEC_PARAMS_NEW, "w");
    if (!fp)
    {
        ereport(elevel,
                (errcode_for_file_access(),
                 errmsg("could not write to file \"%s\": %m",
                        CONFIG_EXEC_PARAMS_NEW)));
        return;
    }

    for (i = 0; i < num_guc_variables; i++)
    {
        write_one_nondefault_variable(fp, guc_variables[i]);
    }

    if (FreeFile(fp))
    {
        ereport(elevel,
                (errcode_for_file_access(),
                 errmsg("could not write to file \"%s\": %m",
                        CONFIG_EXEC_PARAMS_NEW)));
        return;
    }

    /*
     * Put new file in place.  This could delay on Win32, but we don't hold
     * any exclusive locks.
     */
    rename(CONFIG_EXEC_PARAMS_NEW, CONFIG_EXEC_PARAMS);
}


/*
 *    Read string, including null byte from file
 *
 *    Return NULL on EOF and nothing read
 */
static char *
read_string_with_null(FILE *fp)
{
    int            i = 0,
                ch,
                maxlen = 256;
    char       *str = NULL;

    do
    {
        if ((ch = fgetc(fp)) == EOF)
        {
            if (i == 0)
                return NULL;
            else
                elog(FATAL, "invalid format of exec config params file");
        }
        if (i == 0)
            str = guc_malloc(FATAL, maxlen);
        else if (i == maxlen)
            str = guc_realloc(FATAL, str, maxlen *= 2);
        str[i++] = ch;
    } while (ch != 0);

    return str;
}


/*
 *    This routine loads a previous postmaster dump of its non-default
 *    settings.
 */
void
read_nondefault_variables(void)
{// #lizard forgives
    FILE       *fp;
    char       *varname,
               *varvalue,
               *varsourcefile;
    int            varsourceline;
    GucSource    varsource;
    GucContext    varscontext;

    /*
     * Assert that PGC_BACKEND/PGC_SU_BACKEND case in set_config_option() will
     * do the right thing.
     */
    Assert(IsInitProcessingMode());

    /*
     * Open file
     */
    fp = AllocateFile(CONFIG_EXEC_PARAMS, "r");
    if (!fp)
    {
        /* File not found is fine */
        if (errno != ENOENT)
            ereport(FATAL,
                    (errcode_for_file_access(),
                     errmsg("could not read from file \"%s\": %m",
                            CONFIG_EXEC_PARAMS)));
        return;
    }

    for (;;)
    {
        struct config_generic *record;

        if ((varname = read_string_with_null(fp)) == NULL)
            break;

        if ((record = find_option(varname, true, FATAL)) == NULL)
            elog(FATAL, "failed to locate variable \"%s\" in exec config params file", varname);

        if ((varvalue = read_string_with_null(fp)) == NULL)
            elog(FATAL, "invalid format of exec config params file");
        if ((varsourcefile = read_string_with_null(fp)) == NULL)
            elog(FATAL, "invalid format of exec config params file");
        if (fread(&varsourceline, 1, sizeof(varsourceline), fp) != sizeof(varsourceline))
            elog(FATAL, "invalid format of exec config params file");
        if (fread(&varsource, 1, sizeof(varsource), fp) != sizeof(varsource))
            elog(FATAL, "invalid format of exec config params file");
        if (fread(&varscontext, 1, sizeof(varscontext), fp) != sizeof(varscontext))
            elog(FATAL, "invalid format of exec config params file");

        (void) set_config_option(varname, varvalue,
                                 varscontext, varsource,
                                 GUC_ACTION_SET, true, 0, true);
        if (varsourcefile[0])
            set_config_sourcefile(varname, varsourcefile, varsourceline);

        free(varname);
        free(varvalue);
        free(varsourcefile);
    }

    FreeFile(fp);
}
#endif                            /* EXEC_BACKEND */

/*
 * can_skip_gucvar:
 * When serializing, determine whether to skip this GUC.  When restoring, the
 * negation of this test determines whether to restore the compiled-in default
 * value before processing serialized values.
 *
 * A PGC_S_DEFAULT setting on the serialize side will typically match new
 * postmaster children, but that can be false when got_SIGHUP == true and the
 * pending configuration change modifies this setting.  Nonetheless, we omit
 * PGC_S_DEFAULT settings from serialization and make up for that by restoring
 * defaults before applying serialized values.
 *
 * PGC_POSTMASTER variables always have the same value in every child of a
 * particular postmaster.  Most PGC_INTERNAL variables are compile-time
 * constants; a few, like server_encoding and lc_ctype, are handled specially
 * outside the serialize/restore procedure.  Therefore, SerializeGUCState()
 * never sends these, and RestoreGUCState() never changes them.
 */
static bool
can_skip_gucvar(struct config_generic *gconf)
{
    return gconf->context == PGC_POSTMASTER ||
        gconf->context == PGC_INTERNAL || gconf->source == PGC_S_DEFAULT;
}

/*
 * estimate_variable_size:
 *        Compute space needed for dumping the given GUC variable.
 *
 * It's OK to overestimate, but not to underestimate.
 */
static Size
estimate_variable_size(struct config_generic *gconf)
{// #lizard forgives
    Size        size;
    Size        valsize = 0;

    if (can_skip_gucvar(gconf))
        return 0;

    /* Name, plus trailing zero byte. */
    size = strlen(gconf->name) + 1;

    /* Get the maximum display length of the GUC value. */
    switch (gconf->vartype)
    {
        case PGC_BOOL:
            {
                valsize = 5;    /* max(strlen('true'), strlen('false')) */
            }
            break;

        case PGC_INT:
            {
                struct config_int *conf = (struct config_int *) gconf;

                /*
                 * Instead of getting the exact display length, use max
                 * length.  Also reduce the max length for typical ranges of
                 * small values.  Maximum value is 2147483647, i.e. 10 chars.
                 * Include one byte for sign.
                 */
                if (Abs(*conf->variable) < 1000)
                    valsize = 3 + 1;
                else
                    valsize = 10 + 1;
            }
            break;

        case PGC_UINT:
            {
                struct config_uint *conf = (struct config_uint *) gconf;

                /*
                 * Instead of getting the exact display length, use max
                 * length.  Also reduce the max length for typical ranges of
                 * small values.  Maximum value is 2147483647, i.e. 10 chars.
                 * Do not include one byte for sign, as this is unsigned.
                 */
                if (Abs(*conf->variable) < 1000)
                    valsize = 3;
                else
                    valsize = 10;
            }
            break;

        case PGC_REAL:
            {
                /*
                 * We are going to print it with %e with REALTYPE_PRECISION
                 * fractional digits.  Account for sign, leading digit,
                 * decimal point, and exponent with up to 3 digits.  E.g.
                 * -3.99329042340000021e+110
                 */
                valsize = 1 + 1 + 1 + REALTYPE_PRECISION + 5;
            }
            break;

        case PGC_STRING:
            {
                struct config_string *conf = (struct config_string *) gconf;

                /*
                 * If the value is NULL, we transmit it as an empty string.
                 * Although this is not physically the same value, GUC
                 * generally treats a NULL the same as empty string.
                 */
                if (*conf->variable)
                    valsize = strlen(*conf->variable);
                else
                    valsize = 0;
            }
            break;

        case PGC_ENUM:
            {
                struct config_enum *conf = (struct config_enum *) gconf;

                valsize = strlen(config_enum_lookup_by_value(conf, *conf->variable));
            }
            break;
    }

    /* Allow space for terminating zero-byte for value */
    size = add_size(size, valsize + 1);

    if (gconf->sourcefile)
        size = add_size(size, strlen(gconf->sourcefile));

    /* Allow space for terminating zero-byte for sourcefile */
    size = add_size(size, 1);

    /* Include line whenever file is nonempty. */
    if (gconf->sourcefile && gconf->sourcefile[0])
        size = add_size(size, sizeof(gconf->sourceline));

    size = add_size(size, sizeof(gconf->source));
    size = add_size(size, sizeof(gconf->scontext));

    return size;
}

/*
 * EstimateGUCStateSpace:
 * Returns the size needed to store the GUC state for the current process
 */
Size
EstimateGUCStateSpace(void)
{
    Size        size;
    int            i;

    /* Add space reqd for saving the data size of the guc state */
    size = sizeof(Size);

    /* Add up the space needed for each GUC variable */
    for (i = 0; i < num_guc_variables; i++)
        size = add_size(size,
                        estimate_variable_size(guc_variables[i]));

    return size;
}

/*
 * do_serialize:
 * Copies the formatted string into the destination.  Moves ahead the
 * destination pointer, and decrements the maxbytes by that many bytes. If
 * maxbytes is not sufficient to copy the string, error out.
 */
static void
do_serialize(char **destptr, Size *maxbytes, const char *fmt,...)
{
    va_list        vargs;
    int            n;

    if (*maxbytes <= 0)
        elog(ERROR, "not enough space to serialize GUC state");

    va_start(vargs, fmt);
    n = vsnprintf(*destptr, *maxbytes, fmt, vargs);
    va_end(vargs);

    /*
     * Cater to portability hazards in the vsnprintf() return value just like
     * appendPQExpBufferVA() does.  Note that this requires an extra byte of
     * slack at the end of the buffer.  Since serialize_variable() ends with a
     * do_serialize_binary() rather than a do_serialize(), we'll always have
     * that slack; estimate_variable_size() need not add a byte for it.
     */
    if (n < 0 || n >= *maxbytes - 1)
    {
        if (n < 0 && errno != 0 && errno != ENOMEM)
            /* Shouldn't happen. Better show errno description. */
            elog(ERROR, "vsnprintf failed: %m");
        else
            elog(ERROR, "not enough space to serialize GUC state");
    }

    /* Shift the destptr ahead of the null terminator */
    *destptr += n + 1;
    *maxbytes -= n + 1;
}

/* Binary copy version of do_serialize() */
static void
do_serialize_binary(char **destptr, Size *maxbytes, void *val, Size valsize)
{
    if (valsize > *maxbytes)
        elog(ERROR, "not enough space to serialize GUC state");

    memcpy(*destptr, val, valsize);
    *destptr += valsize;
    *maxbytes -= valsize;
}

/*
 * serialize_variable:
 * Dumps name, value and other information of a GUC variable into destptr.
 */
static void
serialize_variable(char **destptr, Size *maxbytes,
                   struct config_generic *gconf)
{// #lizard forgives
    if (can_skip_gucvar(gconf))
        return;

    do_serialize(destptr, maxbytes, "%s", gconf->name);

    switch (gconf->vartype)
    {
        case PGC_BOOL:
            {
                struct config_bool *conf = (struct config_bool *) gconf;

                do_serialize(destptr, maxbytes,
                             (*conf->variable ? "true" : "false"));
            }
            break;

        case PGC_INT:
            {
                struct config_int *conf = (struct config_int *) gconf;

                do_serialize(destptr, maxbytes, "%d", *conf->variable);
            }
            break;

        case PGC_UINT:
            {
                struct config_uint *conf = (struct config_uint *) gconf;

                do_serialize(destptr, maxbytes, "%u", *conf->variable);
            }
            break;

        case PGC_REAL:
            {
                struct config_real *conf = (struct config_real *) gconf;

                do_serialize(destptr, maxbytes, "%.*e",
                             REALTYPE_PRECISION, *conf->variable);
            }
            break;

        case PGC_STRING:
            {
                struct config_string *conf = (struct config_string *) gconf;

                /* NULL becomes empty string, see estimate_variable_size() */
                do_serialize(destptr, maxbytes, "%s",
                             *conf->variable ? *conf->variable : "");
            }
            break;

        case PGC_ENUM:
            {
                struct config_enum *conf = (struct config_enum *) gconf;

                do_serialize(destptr, maxbytes, "%s",
                             config_enum_lookup_by_value(conf, *conf->variable));
            }
            break;
    }

    do_serialize(destptr, maxbytes, "%s",
                 (gconf->sourcefile ? gconf->sourcefile : ""));

    if (gconf->sourcefile && gconf->sourcefile[0])
        do_serialize_binary(destptr, maxbytes, &gconf->sourceline,
                            sizeof(gconf->sourceline));

    do_serialize_binary(destptr, maxbytes, &gconf->source,
                        sizeof(gconf->source));
    do_serialize_binary(destptr, maxbytes, &gconf->scontext,
                        sizeof(gconf->scontext));
}

/*
 * SerializeGUCState:
 * Dumps the complete GUC state onto the memory location at start_address.
 */
void
SerializeGUCState(Size maxsize, char *start_address)
{
    char       *curptr;
    Size        actual_size;
    Size        bytes_left;
    int            i;
    int            i_role = -1;

    /* Reserve space for saving the actual size of the guc state */
    Assert(maxsize > sizeof(actual_size));
    curptr = start_address + sizeof(actual_size);
    bytes_left = maxsize - sizeof(actual_size);

    for (i = 0; i < num_guc_variables; i++)
    {
        /*
         * It's pretty ugly, but we've got to force "role" to be initialized
         * after "session_authorization"; otherwise, the latter will override
         * the former.
         */
        if (strcmp(guc_variables[i]->name, "role") == 0)
        {
#ifndef __TBASE__
            i_role = i;
#else
            {
                struct config_string *conf = (struct config_string *) guc_variables[i];

                /* check the role */
                if (strcmp(*conf->variable, "none") != 0)
                {
                    /* Look up the username */
                    HeapTuple roleTup = SearchSysCache1(AUTHNAME, PointerGetDatum(*conf->variable));
                    if (HeapTupleIsValid(roleTup))
                    {
                        i_role = i;
                    }
                }
            }
#endif
        }
        else
            serialize_variable(&curptr, &bytes_left, guc_variables[i]);
    }
    if (i_role >= 0)
        serialize_variable(&curptr, &bytes_left, guc_variables[i_role]);

    /* Store actual size without assuming alignment of start_address. */
    actual_size = maxsize - bytes_left - sizeof(actual_size);
    memcpy(start_address, &actual_size, sizeof(actual_size));
}

/*
 * read_gucstate:
 * Actually it does not read anything, just returns the srcptr. But it does
 * move the srcptr past the terminating zero byte, so that the caller is ready
 * to read the next string.
 */
static char *
read_gucstate(char **srcptr, char *srcend)
{
    char       *retptr = *srcptr;
    char       *ptr;

    if (*srcptr >= srcend)
        elog(ERROR, "incomplete GUC state");

    /* The string variables are all null terminated */
    for (ptr = *srcptr; ptr < srcend && *ptr != '\0'; ptr++)
        ;

    if (ptr >= srcend)
        elog(ERROR, "could not find null terminator in GUC state");

    /* Set the new position to the byte following the terminating NUL */
    *srcptr = ptr + 1;

    return retptr;
}

/* Binary read version of read_gucstate(). Copies into dest */
static void
read_gucstate_binary(char **srcptr, char *srcend, void *dest, Size size)
{
    if (*srcptr + size > srcend)
        elog(ERROR, "incomplete GUC state");

    memcpy(dest, *srcptr, size);
    *srcptr += size;
}

/*
 * RestoreGUCState:
 * Reads the GUC state at the specified address and updates the GUCs with the
 * values read from the GUC state.
 */
void
RestoreGUCState(void *gucstate)
{
    char       *varname,
               *varvalue,
               *varsourcefile;
    int            varsourceline;
    GucSource    varsource;
    GucContext    varscontext;
    char       *srcptr = (char *) gucstate;
    char       *srcend;
    Size        len;
    int            i;

    /* See comment at can_skip_gucvar(). */
    for (i = 0; i < num_guc_variables; i++)
        if (!can_skip_gucvar(guc_variables[i]))
            InitializeOneGUCOption(guc_variables[i]);

    /* First item is the length of the subsequent data */
    memcpy(&len, gucstate, sizeof(len));

    srcptr += sizeof(len);
    srcend = srcptr + len;

    while (srcptr < srcend)
    {
        int            result;

        varname = read_gucstate(&srcptr, srcend);
        varvalue = read_gucstate(&srcptr, srcend);
        varsourcefile = read_gucstate(&srcptr, srcend);
        if (varsourcefile[0])
            read_gucstate_binary(&srcptr, srcend,
                                 &varsourceline, sizeof(varsourceline));
        read_gucstate_binary(&srcptr, srcend,
                             &varsource, sizeof(varsource));
        read_gucstate_binary(&srcptr, srcend,
                             &varscontext, sizeof(varscontext));

        result = set_config_option(varname, varvalue, varscontext, varsource,
                                   GUC_ACTION_SET, true, ERROR, true);
        if (result <= 0)
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("parameter \"%s\" could not be set", varname)));
        if (varsourcefile[0])
            set_config_sourcefile(varname, varsourcefile, varsourceline);
    }
}

/*
 * A little "long argument" simulation, although not quite GNU
 * compliant. Takes a string of the form "some-option=some value" and
 * returns name = "some_option" and value = "some value" in malloc'ed
 * storage. Note that '-' is converted to '_' in the option name. If
 * there is no '=' in the input string then value will be NULL.
 */
void
ParseLongOption(const char *string, char **name, char **value)
{
    size_t        equal_pos;
    char       *cp;

    AssertArg(string);
    AssertArg(name);
    AssertArg(value);

    equal_pos = strcspn(string, "=");

    if (string[equal_pos] == '=')
    {
        *name = guc_malloc(FATAL, equal_pos + 1);
        strlcpy(*name, string, equal_pos + 1);

        *value = guc_strdup(FATAL, &string[equal_pos + 1]);
    }
    else
    {
        /* no equal sign in string */
        *name = guc_strdup(FATAL, string);
        *value = NULL;
    }

    for (cp = *name; *cp; cp++)
        if (*cp == '-')
            *cp = '_';
}


/*
 * Handle options fetched from pg_db_role_setting.setconfig,
 * pg_proc.proconfig, etc.  Caller must specify proper context/source/action.
 *
 * The array parameter must be an array of TEXT (it must not be NULL).
 */
void
ProcessGUCArray(ArrayType *array,
                GucContext context, GucSource source, GucAction action)
{
    int            i;

    Assert(array != NULL);
    Assert(ARR_ELEMTYPE(array) == TEXTOID);
    Assert(ARR_NDIM(array) == 1);
    Assert(ARR_LBOUND(array)[0] == 1);

    for (i = 1; i <= ARR_DIMS(array)[0]; i++)
    {
        Datum        d;
        bool        isnull;
        char       *s;
        char       *name;
        char       *value;

        d = array_ref(array, 1, &i,
                      -1 /* varlenarray */ ,
                      -1 /* TEXT's typlen */ ,
                      false /* TEXT's typbyval */ ,
                      'i' /* TEXT's typalign */ ,
                      &isnull);

        if (isnull)
            continue;

        s = TextDatumGetCString(d);

        ParseLongOption(s, &name, &value);
        if (!value)
        {
            ereport(WARNING,
                    (errcode(ERRCODE_SYNTAX_ERROR),
                     errmsg("could not parse setting for parameter \"%s\"",
                            name)));
            free(name);
            continue;
        }

        (void) set_config_option(name, value,
                                 context, source,
                                 action, true, 0, false);

        free(name);
        if (value)
            free(value);
        pfree(s);
    }
}


/*
 * Add an entry to an option array.  The array parameter may be NULL
 * to indicate the current table entry is NULL.
 */
ArrayType *
GUCArrayAdd(ArrayType *array, const char *name, const char *value)
{
    struct config_generic *record;
    Datum        datum;
    char       *newval;
    ArrayType  *a;

    Assert(name);
    Assert(value);

    /* test if the option is valid and we're allowed to set it */
    (void) validate_option_array_item(name, value, false);

    /* normalize name (converts obsolete GUC names to modern spellings) */
    record = find_option(name, false, WARNING);
    if (record)
        name = record->name;

    /* build new item for array */
    newval = psprintf("%s=%s", name, value);
    datum = CStringGetTextDatum(newval);

    if (array)
    {
        int            index;
        bool        isnull;
        int            i;

        Assert(ARR_ELEMTYPE(array) == TEXTOID);
        Assert(ARR_NDIM(array) == 1);
        Assert(ARR_LBOUND(array)[0] == 1);

        index = ARR_DIMS(array)[0] + 1; /* add after end */

        for (i = 1; i <= ARR_DIMS(array)[0]; i++)
        {
            Datum        d;
            char       *current;

            d = array_ref(array, 1, &i,
                          -1 /* varlenarray */ ,
                          -1 /* TEXT's typlen */ ,
                          false /* TEXT's typbyval */ ,
                          'i' /* TEXT's typalign */ ,
                          &isnull);
            if (isnull)
                continue;
            current = TextDatumGetCString(d);

            /* check for match up through and including '=' */
            if (strncmp(current, newval, strlen(name) + 1) == 0)
            {
                index = i;
                break;
            }
        }

        a = array_set(array, 1, &index,
                      datum,
                      false,
                      -1 /* varlena array */ ,
                      -1 /* TEXT's typlen */ ,
                      false /* TEXT's typbyval */ ,
                      'i' /* TEXT's typalign */ );
    }
    else
        a = construct_array(&datum, 1,
                            TEXTOID,
                            -1, false, 'i');

    return a;
}


/*
 * Delete an entry from an option array.  The array parameter may be NULL
 * to indicate the current table entry is NULL.  Also, if the return value
 * is NULL then a null should be stored.
 */
ArrayType *
GUCArrayDelete(ArrayType *array, const char *name)
{// #lizard forgives
    struct config_generic *record;
    ArrayType  *newarray;
    int            i;
    int            index;

    Assert(name);

    /* test if the option is valid and we're allowed to set it */
    (void) validate_option_array_item(name, NULL, false);

    /* normalize name (converts obsolete GUC names to modern spellings) */
    record = find_option(name, false, WARNING);
    if (record)
        name = record->name;

    /* if array is currently null, then surely nothing to delete */
    if (!array)
        return NULL;

    newarray = NULL;
    index = 1;

    for (i = 1; i <= ARR_DIMS(array)[0]; i++)
    {
        Datum        d;
        char       *val;
        bool        isnull;

        d = array_ref(array, 1, &i,
                      -1 /* varlenarray */ ,
                      -1 /* TEXT's typlen */ ,
                      false /* TEXT's typbyval */ ,
                      'i' /* TEXT's typalign */ ,
                      &isnull);
        if (isnull)
            continue;
        val = TextDatumGetCString(d);

        /* ignore entry if it's what we want to delete */
        if (strncmp(val, name, strlen(name)) == 0
            && val[strlen(name)] == '=')
            continue;

        /* else add it to the output array */
        if (newarray)
            newarray = array_set(newarray, 1, &index,
                                 d,
                                 false,
                                 -1 /* varlenarray */ ,
                                 -1 /* TEXT's typlen */ ,
                                 false /* TEXT's typbyval */ ,
                                 'i' /* TEXT's typalign */ );
        else
            newarray = construct_array(&d, 1,
                                       TEXTOID,
                                       -1, false, 'i');

        index++;
    }

    return newarray;
}


/*
 * Given a GUC array, delete all settings from it that our permission
 * level allows: if superuser, delete them all; if regular user, only
 * those that are PGC_USERSET
 */
ArrayType *
GUCArrayReset(ArrayType *array)
{
    ArrayType  *newarray;
    int            i;
    int            index;

    /* if array is currently null, nothing to do */
    if (!array)
        return NULL;

    /* if we're superuser, we can delete everything, so just do it */
    if (superuser())
        return NULL;

    newarray = NULL;
    index = 1;

    for (i = 1; i <= ARR_DIMS(array)[0]; i++)
    {
        Datum        d;
        char       *val;
        char       *eqsgn;
        bool        isnull;

        d = array_ref(array, 1, &i,
                      -1 /* varlenarray */ ,
                      -1 /* TEXT's typlen */ ,
                      false /* TEXT's typbyval */ ,
                      'i' /* TEXT's typalign */ ,
                      &isnull);
        if (isnull)
            continue;
        val = TextDatumGetCString(d);

        eqsgn = strchr(val, '=');
        *eqsgn = '\0';

        /* skip if we have permission to delete it */
        if (validate_option_array_item(val, NULL, true))
            continue;

        /* else add it to the output array */
        if (newarray)
            newarray = array_set(newarray, 1, &index,
                                 d,
                                 false,
                                 -1 /* varlenarray */ ,
                                 -1 /* TEXT's typlen */ ,
                                 false /* TEXT's typbyval */ ,
                                 'i' /* TEXT's typalign */ );
        else
            newarray = construct_array(&d, 1,
                                       TEXTOID,
                                       -1, false, 'i');

        index++;
        pfree(val);
    }

    return newarray;
}

/*
 * Validate a proposed option setting for GUCArrayAdd/Delete/Reset.
 *
 * name is the option name.  value is the proposed value for the Add case,
 * or NULL for the Delete/Reset cases.  If skipIfNoPermissions is true, it's
 * not an error to have no permissions to set the option.
 *
 * Returns TRUE if OK, FALSE if skipIfNoPermissions is true and user does not
 * have permission to change this option (all other error cases result in an
 * error being thrown).
 */
static bool
validate_option_array_item(const char *name, const char *value,
                           bool skipIfNoPermissions)

{// #lizard forgives
    struct config_generic *gconf;

    /*
     * There are three cases to consider:
     *
     * name is a known GUC variable.  Check the value normally, check
     * permissions normally (i.e., allow if variable is USERSET, or if it's
     * SUSET and user is superuser).
     *
     * name is not known, but exists or can be created as a placeholder (i.e.,
     * it has a prefixed name).  We allow this case if you're a superuser,
     * otherwise not.  Superusers are assumed to know what they're doing. We
     * can't allow it for other users, because when the placeholder is
     * resolved it might turn out to be a SUSET variable;
     * define_custom_variable assumes we checked that.
     *
     * name is not known and can't be created as a placeholder.  Throw error,
     * unless skipIfNoPermissions is true, in which case return FALSE.
     */
    gconf = find_option(name, true, WARNING);
    if (!gconf)
    {
        /* not known, failed to make a placeholder */
        if (skipIfNoPermissions)
            return false;
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_OBJECT),
                 errmsg("unrecognized configuration parameter \"%s\"",
                        name)));
    }

    if (gconf->flags & GUC_CUSTOM_PLACEHOLDER)
    {
        /*
         * We cannot do any meaningful check on the value, so only permissions
         * are useful to check.
         */
        if (superuser())
            return true;
        if (skipIfNoPermissions)
            return false;
        ereport(ERROR,
                (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                 errmsg("permission denied to set parameter \"%s\"", name)));
    }

    /* manual permissions check so we can avoid an error being thrown */
    if (gconf->context == PGC_USERSET)
         /* ok */ ;
    else if (gconf->context == PGC_SUSET && superuser())
         /* ok */ ;
    else if (skipIfNoPermissions)
        return false;
    /* if a permissions error should be thrown, let set_config_option do it */

    /* test for permissions and valid option value */
    (void) set_config_option(name, value,
                             superuser() ? PGC_SUSET : PGC_USERSET,
                             PGC_S_TEST, GUC_ACTION_SET, false, 0, false);

    return true;
}


/*
 * Called by check_hooks that want to override the normal
 * ERRCODE_INVALID_PARAMETER_VALUE SQLSTATE for check hook failures.
 *
 * Note that GUC_check_errmsg() etc are just macros that result in a direct
 * assignment to the associated variables.  That is ugly, but forced by the
 * limitations of C's macro mechanisms.
 */
void
GUC_check_errcode(int sqlerrcode)
{
    GUC_check_errcode_value = sqlerrcode;
}


/*
 * Convenience functions to manage calling a variable's check_hook.
 * These mostly take care of the protocol for letting check hooks supply
 * portions of the error report on failure.
 */

static bool
call_bool_check_hook(struct config_bool *conf, bool *newval, void **extra,
                     GucSource source, int elevel)
{
    /* Quick success if no hook */
    if (!conf->check_hook)
        return true;

    /* Reset variables that might be set by hook */
    GUC_check_errcode_value = ERRCODE_INVALID_PARAMETER_VALUE;
    GUC_check_errmsg_string = NULL;
    GUC_check_errdetail_string = NULL;
    GUC_check_errhint_string = NULL;

    if (!(*conf->check_hook) (newval, extra, source))
    {
        ereport(elevel,
                (errcode(GUC_check_errcode_value),
                 GUC_check_errmsg_string ?
                 errmsg_internal("%s", GUC_check_errmsg_string) :
                 errmsg("invalid value for parameter \"%s\": %d",
                        conf->gen.name, (int) *newval),
                 GUC_check_errdetail_string ?
                 errdetail_internal("%s", GUC_check_errdetail_string) : 0,
                 GUC_check_errhint_string ?
                 errhint("%s", GUC_check_errhint_string) : 0));
        /* Flush any strings created in ErrorContext */
        FlushErrorState();
        return false;
    }

    return true;
}

static bool
call_int_check_hook(struct config_int *conf, int *newval, void **extra,
                    GucSource source, int elevel)
{
    /* Quick success if no hook */
    if (!conf->check_hook)
        return true;

    /* Reset variables that might be set by hook */
    GUC_check_errcode_value = ERRCODE_INVALID_PARAMETER_VALUE;
    GUC_check_errmsg_string = NULL;
    GUC_check_errdetail_string = NULL;
    GUC_check_errhint_string = NULL;

    if (!(*conf->check_hook) (newval, extra, source))
    {
        ereport(elevel,
                (errcode(GUC_check_errcode_value),
                 GUC_check_errmsg_string ?
                 errmsg_internal("%s", GUC_check_errmsg_string) :
                 errmsg("invalid value for parameter \"%s\": %d",
                        conf->gen.name, *newval),
                 GUC_check_errdetail_string ?
                 errdetail_internal("%s", GUC_check_errdetail_string) : 0,
                 GUC_check_errhint_string ?
                 errhint("%s", GUC_check_errhint_string) : 0));
        /* Flush any strings created in ErrorContext */
        FlushErrorState();
        return false;
    }

    return true;
}

static bool
call_uint_check_hook(struct config_uint *conf, uint *newval, void **extra,
                     GucSource source, int elevel)
{
    /* Quick success if no hook */
    if (!conf->check_hook)
        return true;

    /* Reset variables that might be set by hook */
    GUC_check_errcode_value = ERRCODE_INVALID_PARAMETER_VALUE;
    GUC_check_errmsg_string = NULL;
    GUC_check_errdetail_string = NULL;
    GUC_check_errhint_string = NULL;

    if (!(*conf->check_hook) (newval, extra, source))
    {
        ereport(elevel,
                (errcode(GUC_check_errcode_value),
                 GUC_check_errmsg_string ?
                 errmsg_internal("%s", GUC_check_errmsg_string) :
                 errmsg("invalid value for parameter \"%s\": %d",
                        conf->gen.name, *newval),
                 GUC_check_errdetail_string ?
                 errdetail_internal("%s", GUC_check_errdetail_string) : 0,
                 GUC_check_errhint_string ?
                 errhint("%s", GUC_check_errhint_string) : 0));
        /* Flush any strings created in ErrorContext */
        FlushErrorState();
        return false;
    }

    return true;
}

static bool
call_real_check_hook(struct config_real *conf, double *newval, void **extra,
                     GucSource source, int elevel)
{
    /* Quick success if no hook */
    if (!conf->check_hook)
        return true;

    /* Reset variables that might be set by hook */
    GUC_check_errcode_value = ERRCODE_INVALID_PARAMETER_VALUE;
    GUC_check_errmsg_string = NULL;
    GUC_check_errdetail_string = NULL;
    GUC_check_errhint_string = NULL;

    if (!(*conf->check_hook) (newval, extra, source))
    {
        ereport(elevel,
                (errcode(GUC_check_errcode_value),
                 GUC_check_errmsg_string ?
                 errmsg_internal("%s", GUC_check_errmsg_string) :
                 errmsg("invalid value for parameter \"%s\": %g",
                        conf->gen.name, *newval),
                 GUC_check_errdetail_string ?
                 errdetail_internal("%s", GUC_check_errdetail_string) : 0,
                 GUC_check_errhint_string ?
                 errhint("%s", GUC_check_errhint_string) : 0));
        /* Flush any strings created in ErrorContext */
        FlushErrorState();
        return false;
    }

    return true;
}

static bool
call_string_check_hook(struct config_string *conf, char **newval, void **extra,
                       GucSource source, int elevel)
{
    /* Quick success if no hook */
    if (!conf->check_hook)
        return true;

    /* Reset variables that might be set by hook */
    GUC_check_errcode_value = ERRCODE_INVALID_PARAMETER_VALUE;
    GUC_check_errmsg_string = NULL;
    GUC_check_errdetail_string = NULL;
    GUC_check_errhint_string = NULL;

    if (!(*conf->check_hook) (newval, extra, source))
    {
        ereport(elevel,
                (errcode(GUC_check_errcode_value),
                 GUC_check_errmsg_string ?
                 errmsg_internal("%s", GUC_check_errmsg_string) :
                 errmsg("invalid value for parameter \"%s\": \"%s\"",
                        conf->gen.name, *newval ? *newval : ""),
                 GUC_check_errdetail_string ?
                 errdetail_internal("%s", GUC_check_errdetail_string) : 0,
                 GUC_check_errhint_string ?
                 errhint("%s", GUC_check_errhint_string) : 0));
        /* Flush any strings created in ErrorContext */
        FlushErrorState();
        return false;
    }

    return true;
}

static bool
call_enum_check_hook(struct config_enum *conf, int *newval, void **extra,
                     GucSource source, int elevel)
{
    /* Quick success if no hook */
    if (!conf->check_hook)
        return true;

    /* Reset variables that might be set by hook */
    GUC_check_errcode_value = ERRCODE_INVALID_PARAMETER_VALUE;
    GUC_check_errmsg_string = NULL;
    GUC_check_errdetail_string = NULL;
    GUC_check_errhint_string = NULL;

    if (!(*conf->check_hook) (newval, extra, source))
    {
        ereport(elevel,
                (errcode(GUC_check_errcode_value),
                 GUC_check_errmsg_string ?
                 errmsg_internal("%s", GUC_check_errmsg_string) :
                 errmsg("invalid value for parameter \"%s\": \"%s\"",
                        conf->gen.name,
                        config_enum_lookup_by_value(conf, *newval)),
                 GUC_check_errdetail_string ?
                 errdetail_internal("%s", GUC_check_errdetail_string) : 0,
                 GUC_check_errhint_string ?
                 errhint("%s", GUC_check_errhint_string) : 0));
        /* Flush any strings created in ErrorContext */
        FlushErrorState();
        return false;
    }

    return true;
}


/*
 * check_hook, assign_hook and show_hook subroutines
 */

static bool
check_wal_consistency_checking(char **newval, void **extra, GucSource source)
{// #lizard forgives
    char       *rawstring;
    List       *elemlist;
    ListCell   *l;
    bool        newwalconsistency[RM_MAX_ID + 1];

    /* Initialize the array */
    MemSet(newwalconsistency, 0, (RM_MAX_ID + 1) * sizeof(bool));

    /* Need a modifiable copy of string */
    rawstring = pstrdup(*newval);

    /* Parse string into list of identifiers */
    if (!SplitIdentifierString(rawstring, ',', &elemlist))
    {
        /* syntax error in list */
        GUC_check_errdetail("List syntax is invalid.");
        pfree(rawstring);
        list_free(elemlist);
        return false;
    }

    foreach(l, elemlist)
    {
        char       *tok = (char *) lfirst(l);
        bool        found = false;
        RmgrId        rmid;

        /* Check for 'all'. */
        if (pg_strcasecmp(tok, "all") == 0)
        {
            for (rmid = 0; rmid <= RM_MAX_ID; rmid++)
                if (RmgrTable[rmid].rm_mask != NULL)
                    newwalconsistency[rmid] = true;
            found = true;
        }
        else
        {
            /*
             * Check if the token matches with any individual resource
             * manager.
             */
            for (rmid = 0; rmid <= RM_MAX_ID; rmid++)
            {
                if (pg_strcasecmp(tok, RmgrTable[rmid].rm_name) == 0 &&
                    RmgrTable[rmid].rm_mask != NULL)
                {
                    newwalconsistency[rmid] = true;
                    found = true;
                }
            }
        }

        /* If a valid resource manager is found, check for the next one. */
        if (!found)
        {
            GUC_check_errdetail("Unrecognized key word: \"%s\".", tok);
            pfree(rawstring);
            list_free(elemlist);
            return false;
        }
    }

    pfree(rawstring);
    list_free(elemlist);

    /* assign new value */
    *extra = guc_malloc(ERROR, (RM_MAX_ID + 1) * sizeof(bool));
    memcpy(*extra, newwalconsistency, (RM_MAX_ID + 1) * sizeof(bool));
    return true;
}

static void
assign_wal_consistency_checking(const char *newval, void *extra)
{
    wal_consistency_checking = (bool *) extra;
}

static bool
check_log_destination(char **newval, void **extra, GucSource source)
{// #lizard forgives
    char       *rawstring;
    List       *elemlist;
    ListCell   *l;
    int            newlogdest = 0;
    int           *myextra;

    /* Need a modifiable copy of string */
    rawstring = pstrdup(*newval);

    /* Parse string into list of identifiers */
    if (!SplitIdentifierString(rawstring, ',', &elemlist))
    {
        /* syntax error in list */
        GUC_check_errdetail("List syntax is invalid.");
        pfree(rawstring);
        list_free(elemlist);
        return false;
    }

    foreach(l, elemlist)
    {
        char       *tok = (char *) lfirst(l);

        if (pg_strcasecmp(tok, "stderr") == 0)
            newlogdest |= LOG_DESTINATION_STDERR;
        else if (pg_strcasecmp(tok, "csvlog") == 0)
            newlogdest |= LOG_DESTINATION_CSVLOG;
#ifdef HAVE_SYSLOG
        else if (pg_strcasecmp(tok, "syslog") == 0)
            newlogdest |= LOG_DESTINATION_SYSLOG;
#endif
#ifdef WIN32
        else if (pg_strcasecmp(tok, "eventlog") == 0)
            newlogdest |= LOG_DESTINATION_EVENTLOG;
#endif
        else
        {
            GUC_check_errdetail("Unrecognized key word: \"%s\".", tok);
            pfree(rawstring);
            list_free(elemlist);
            return false;
        }
    }

    pfree(rawstring);
    list_free(elemlist);

    myextra = (int *) guc_malloc(ERROR, sizeof(int));
    *myextra = newlogdest;
    *extra = (void *) myextra;

    return true;
}

static void
assign_log_destination(const char *newval, void *extra)
{
    Log_destination = *((int *) extra);
}

static void
assign_syslog_facility(int newval, void *extra)
{
#ifdef HAVE_SYSLOG
    set_syslog_parameters(syslog_ident_str ? syslog_ident_str : "postgres",
                          newval);
#endif
    /* Without syslog support, just ignore it */
}

static void
assign_syslog_ident(const char *newval, void *extra)
{
#ifdef HAVE_SYSLOG
    set_syslog_parameters(newval, syslog_facility);
#endif
    /* Without syslog support, it will always be set to "none", so ignore */
}


static void
assign_session_replication_role(int newval, void *extra)
{
    /*
     * Must flush the plan cache when changing replication role; but don't
     * flush unnecessarily.
     */
    if (SessionReplicationRole != newval)
        ResetPlanCache();
}

static bool
check_temp_buffers(int *newval, void **extra, GucSource source)
{
    /*
     * Once local buffers have been initialized, it's too late to change this.
     */
    if (NLocBuffer && NLocBuffer != *newval)
    {
        GUC_check_errdetail("\"temp_buffers\" cannot be changed after any temporary tables have been accessed in the session.");
        return false;
    }
    return true;
}

static bool
check_bonjour(bool *newval, void **extra, GucSource source)
{
#ifndef USE_BONJOUR
    if (*newval)
    {
        GUC_check_errmsg("Bonjour is not supported by this build");
        return false;
    }
#endif
    return true;
}

static bool
check_ssl(bool *newval, void **extra, GucSource source)
{
#ifndef USE_SSL
    if (*newval)
    {
        GUC_check_errmsg("SSL is not supported by this build");
        return false;
    }
#endif
    return true;
}

static bool
check_stage_log_stats(bool *newval, void **extra, GucSource source)
{
    if (*newval && log_statement_stats)
    {
        GUC_check_errdetail("Cannot enable parameter when \"log_statement_stats\" is true.");
        return false;
    }
    return true;
}

static bool
check_log_stats(bool *newval, void **extra, GucSource source)
{
    if (*newval &&
        (log_parser_stats || log_planner_stats || log_executor_stats))
    {
        GUC_check_errdetail("Cannot enable \"log_statement_stats\" when "
                            "\"log_parser_stats\", \"log_planner_stats\", "
                            "or \"log_executor_stats\" is true.");
        return false;
    }
    return true;
}

#ifdef PGXC
/*
 * Only a warning is printed to log.
 * Returning false will cause FATAL error and it will not be good.
 */
static bool
check_pgxc_maintenance_mode(bool *newval, void **extra, GucSource source)
{// #lizard forgives

    switch(source)
    {
        case PGC_S_DYNAMIC_DEFAULT:
        case PGC_S_ENV_VAR:
        case PGC_S_ARGV:
            GUC_check_errmsg("pgxc_maintenance_mode is not allowed here.");
            return false;
        case PGC_S_FILE:
            switch (currentGucContext)
            {
                case PGC_SIGHUP:
                    elog(WARNING, "pgxc_maintenance_mode is not allowed in  postgresql.conf.  Set to default (false).");
                    *newval = false;
                    return true;
                default:
                    GUC_check_errmsg("pgxc_maintenance_mode is not allowed in postgresql.conf.");
                    return false;
            }
            return false;    /* Should not come here */
        case PGC_S_DATABASE:
        case PGC_S_USER:
        case PGC_S_DATABASE_USER:
        case PGC_S_INTERACTIVE:
        case PGC_S_TEST:
            elog(WARNING, "pgxc_maintenance_mode is not allowed here.  Set to default (false).");
            *newval = false;
            return true;
        case PGC_S_DEFAULT:
        case PGC_S_CLIENT:
        case PGC_S_SESSION:
            return true;
        default:
            GUC_check_errmsg("Unknown source");
            return false;
    }
}
#endif

static bool
check_canonical_path(char **newval, void **extra, GucSource source)
{
    /*
     * Since canonicalize_path never enlarges the string, we can just modify
     * newval in-place.  But watch out for NULL, which is the default value
     * for external_pid_file.
     */
    if (*newval)
        canonicalize_path(*newval);
    return true;
}

static bool
check_timezone_abbreviations(char **newval, void **extra, GucSource source)
{
    /*
     * The boot_val given above for timezone_abbreviations is NULL. When we
     * see this we just do nothing.  If this value isn't overridden from the
     * config file then pg_timezone_abbrev_initialize() will eventually
     * replace it with "Default".  This hack has two purposes: to avoid
     * wasting cycles loading values that might soon be overridden from the
     * config file, and to avoid trying to read the timezone abbrev files
     * during InitializeGUCOptions().  The latter doesn't work in an
     * EXEC_BACKEND subprocess because my_exec_path hasn't been set yet and so
     * we can't locate PGSHAREDIR.
     */
    if (*newval == NULL)
    {
        Assert(source == PGC_S_DEFAULT);
        return true;
    }

    /* OK, load the file and produce a malloc'd TimeZoneAbbrevTable */
    *extra = load_tzoffsets(*newval);

    /* tzparser.c returns NULL on failure, reporting via GUC_check_errmsg */
    if (!*extra)
        return false;

    return true;
}

static void
assign_timezone_abbreviations(const char *newval, void *extra)
{
    /* Do nothing for the boot_val default of NULL */
    if (!extra)
        return;

    InstallTimeZoneAbbrevs((TimeZoneAbbrevTable *) extra);
}

/*
 * pg_timezone_abbrev_initialize --- set default value if not done already
 *
 * This is called after initial loading of postgresql.conf.  If no
 * timezone_abbreviations setting was found therein, select default.
 * If a non-default value is already installed, nothing will happen.
 *
 * This can also be called from ProcessConfigFile to establish the default
 * value after a postgresql.conf entry for it is removed.
 */
static void
pg_timezone_abbrev_initialize(void)
{
    SetConfigOption("timezone_abbreviations", "Default",
                    PGC_POSTMASTER, PGC_S_DYNAMIC_DEFAULT);
}

static const char *
show_archive_command(void)
{
    if (XLogArchivingActive())
        return XLogArchiveCommand;
    else
        return "(disabled)";
}

static void
assign_tcp_keepalives_idle(int newval, void *extra)
{
    /*
     * The kernel API provides no way to test a value without setting it; and
     * once we set it we might fail to unset it.  So there seems little point
     * in fully implementing the check-then-assign GUC API for these
     * variables.  Instead we just do the assignment on demand.  pqcomm.c
     * reports any problems via elog(LOG).
     *
     * This approach means that the GUC value might have little to do with the
     * actual kernel value, so we use a show_hook that retrieves the kernel
     * value rather than trusting GUC's copy.
     */
    (void) pq_setkeepalivesidle(newval, MyProcPort);
}

static const char *
show_tcp_keepalives_idle(void)
{
    /* See comments in assign_tcp_keepalives_idle */
    static char nbuf[16];

    snprintf(nbuf, sizeof(nbuf), "%d", pq_getkeepalivesidle(MyProcPort));
    return nbuf;
}

static void
assign_tcp_keepalives_interval(int newval, void *extra)
{
    /* See comments in assign_tcp_keepalives_idle */
    (void) pq_setkeepalivesinterval(newval, MyProcPort);
}

static const char *
show_tcp_keepalives_interval(void)
{
    /* See comments in assign_tcp_keepalives_idle */
    static char nbuf[16];

    snprintf(nbuf, sizeof(nbuf), "%d", pq_getkeepalivesinterval(MyProcPort));
    return nbuf;
}

static void
assign_tcp_keepalives_count(int newval, void *extra)
{
    /* See comments in assign_tcp_keepalives_idle */
    (void) pq_setkeepalivescount(newval, MyProcPort);
}

static const char *
show_tcp_keepalives_count(void)
{
    /* See comments in assign_tcp_keepalives_idle */
    static char nbuf[16];

    snprintf(nbuf, sizeof(nbuf), "%d", pq_getkeepalivescount(MyProcPort));
    return nbuf;
}

static bool
check_maxconnections(int *newval, void **extra, GucSource source)
{
    if (*newval + autovacuum_max_workers + 1 +
        max_worker_processes > MAX_BACKENDS)
        return false;
    return true;
}

static bool
check_autovacuum_max_workers(int *newval, void **extra, GucSource source)
{
    if (MaxConnections + *newval + 1 + max_worker_processes > MAX_BACKENDS)
        return false;
    return true;
}

static bool
check_autovacuum_work_mem(int *newval, void **extra, GucSource source)
{
    /*
     * -1 indicates fallback.
     *
     * If we haven't yet changed the boot_val default of -1, just let it be.
     * Autovacuum will look to maintenance_work_mem instead.
     */
    if (*newval == -1)
        return true;

    /*
     * We clamp manually-set values to at least 1MB.  Since
     * maintenance_work_mem is always set to at least this value, do the same
     * here.
     */
    if (*newval < 1024)
        *newval = 1024;

    return true;
}

static bool
check_max_worker_processes(int *newval, void **extra, GucSource source)
{
    if (MaxConnections + autovacuum_max_workers + 1 + *newval > MAX_BACKENDS)
        return false;
    return true;
}

static bool
check_effective_io_concurrency(int *newval, void **extra, GucSource source)
{
#ifdef USE_PREFETCH
    double        new_prefetch_pages;

    if (ComputeIoConcurrency(*newval, &new_prefetch_pages))
    {
        int           *myextra = (int *) guc_malloc(ERROR, sizeof(int));

        *myextra = (int) rint(new_prefetch_pages);
        *extra = (void *) myextra;

        return true;
    }
    else
        return false;
#else
    return true;
#endif                            /* USE_PREFETCH */
}

static void
assign_effective_io_concurrency(int newval, void *extra)
{
#ifdef USE_PREFETCH
    target_prefetch_pages = *((int *) extra);
#endif                            /* USE_PREFETCH */
}

static void
assign_pgstat_temp_directory(const char *newval, void *extra)
{
    /* check_canonical_path already canonicalized newval for us */
    char       *dname;
    char       *tname;
    char       *fname;

    /* directory */
    dname = guc_malloc(ERROR, strlen(newval) + 1);    /* runtime dir */
    sprintf(dname, "%s", newval);

    /* global stats */
    tname = guc_malloc(ERROR, strlen(newval) + 12); /* /global.tmp */
    sprintf(tname, "%s/global.tmp", newval);
    fname = guc_malloc(ERROR, strlen(newval) + 13); /* /global.stat */
    sprintf(fname, "%s/global.stat", newval);

    if (pgstat_stat_directory)
        free(pgstat_stat_directory);
    pgstat_stat_directory = dname;
    if (pgstat_stat_tmpname)
        free(pgstat_stat_tmpname);
    pgstat_stat_tmpname = tname;
    if (pgstat_stat_filename)
        free(pgstat_stat_filename);
    pgstat_stat_filename = fname;
}

static bool
check_application_name(char **newval, void **extra, GucSource source)
{
    /* Only allow clean ASCII chars in the application name */
    char       *p;

    for (p = *newval; *p; p++)
    {
        if (*p < 32 || *p > 126)
            *p = '?';
    }

    return true;
}

static void
assign_application_name(const char *newval, void *extra)
{
    /* Update the pg_stat_activity view */
    pgstat_report_appname(newval);
}

static bool
check_cluster_name(char **newval, void **extra, GucSource source)
{
    /* Only allow clean ASCII chars in the cluster name */
    char       *p;

    for (p = *newval; *p; p++)
    {
        if (*p < 32 || *p > 126)
            *p = '?';
    }

    return true;
}

static const char *
show_unix_socket_permissions(void)
{
    static char buf[8];

    snprintf(buf, sizeof(buf), "%04o", Unix_socket_permissions);
    return buf;
}

static const char *
show_log_file_mode(void)
{
    static char buf[8];

    snprintf(buf, sizeof(buf), "%04o", Log_file_mode);
    return buf;
}

#ifdef __AUDIT__
static const char *
show_alog_file_mode(void)
{
    static char buf[8];

    snprintf(buf, sizeof(buf), "%04o", AuditLog_file_mode);
    return buf;
}
#endif

#ifdef XCP
/*
 * Return a quoted GUC value, when necessary
 */
const char *
quote_guc_value(const char *value, int flags)
{
    char *new_value;

    if (value == NULL)
        return value;

    /*
     * An empty string is what gets created when someone fires SET var TO ''.
     * We must send it in its original form to the remote node.
     */
    if (!value[0])
        return "''";

    /*
     * A special case for empty string list members which may get replaced by
     * "\"\"" when flatten_set_variable_args gets called. So replace that back
     * to ''.
     */
    new_value = pstrdup(value);
    strreplace_all(new_value, "\"\"", "''");
    value = new_value;

    /* Finally don't quote empty string */
    if (strcmp(value, "''") == 0)
        return value;

    /*
     * If the GUC rceives list input, then the individual elements in the list
     * must be already quoted correctly by flatten_set_variable_args(). We must
     * not quote the entire value again.
     *
     * We deal with empty strings which may have already been replaced with ""
     * by flatten_set_variable_args. Unquote them so that remote side can
     * handle it.
     */
    if (flags & GUC_LIST_INPUT)
       return value;

    /*
     * Otherwise quote the value. quote_identifier() takes care of correctly
     * quoting the value when needed, including GUC_UNIT_MEMORY and
     * GUC_UNIT_TIME values.
     */
    return quote_identifier(value);
}

/*
 * Replace all occurrences of "needle" with "replacement". We do in-place
 * replacement, so "replacement" must be smaller or equal to "needle"
 */
static void
strreplace_all(char *str, char *needle, char *replacement)
{
    char       *s;

    s = strstr(str, needle);
    while (s != NULL)
    {
        int            replacementlen = strlen(replacement);
        char       *rest = s + strlen(needle);

        memcpy(s, replacement, replacementlen);
        memmove(s + replacementlen, rest, strlen(rest) + 1);
        s = strstr(str, needle);
    }
}
#endif
#ifdef __TBASE__
bool
set_warm_shared_buffer(bool *newval, void **extra, GucSource source)
{
    if (!g_WarmSharedBuffer)
    {
        WarmSharedBuffer();
        g_WarmSharedBuffer = true;
    }
    return true;
}
static const char *
show_total_memorysize(void)
{
    int32   size;
    static char buf[64];
    size = get_total_memory_size();
	snprintf(buf, sizeof(buf), "%dM", size);
    return buf;
}
#endif
#ifdef __COLD_HOT__
static void
assign_cold_hot_partition_type(const char *newval, void *extra)
{// #lizard forgives
    if (newval && newval[0] != '\0')
    {
        if (strcmp(newval, "day") == 0)
        {
            g_ColdHotPartitionType = IntervalType_Day;
        }
        else if (strcmp(newval, "month") == 0)
        {
            g_ColdHotPartitionType = IntervalType_Month;
        }
        else if (strcmp(newval, "year") == 0)
        {
            g_ColdHotPartitionType = IntervalType_Year;
        }
        else
        {
            ereport(ERROR,
                    (errcode(ERRCODE_UNDEFINED_PARAMETER),
                     errmsg("cold_hot_sepration_mode must be day/month/year"))); 
        }

        if (g_ManualHotDate && g_ManualHotDate[0] != '\0')
        {
            fsec_t fsec;
            Datum stamp;
            
            stamp = DirectFunctionCall3(timestamp_in,
                            CStringGetDatum(g_ManualHotDate),
                            ObjectIdGetDatum(InvalidOid),
                            Int32GetDatum(-1));
        
            if(timestamp2tm(stamp, NULL, &g_ManualHotDataTime, &fsec, NULL, NULL) != 0)
            {
                ereport(ERROR,
                            (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                             errmsg("timestamp out of range")));    
            }
            
            if (g_ColdHotPartitionType == IntervalType_Day)
            {
                g_ManualHotDataTime.tm_sec = 0;
                g_ManualHotDataTime.tm_min = 0;
                g_ManualHotDataTime.tm_hour = 0;
            }
            else if (g_ColdHotPartitionType == IntervalType_Month)
            {
                g_ManualHotDataTime.tm_sec = 0;
                g_ManualHotDataTime.tm_min = 0;
                g_ManualHotDataTime.tm_hour = 0;
                g_ManualHotDataTime.tm_mday = 1;
            }
            else if (g_ColdHotPartitionType == IntervalType_Year)
            {
                g_ManualHotDataTime.tm_sec = 0;
                g_ManualHotDataTime.tm_min = 0;
                g_ManualHotDataTime.tm_hour = 0;
                g_ManualHotDataTime.tm_mday = 1;
                g_ManualHotDataTime.tm_mon = 1;
            }
        }
    }  
    else
    {
       g_ColdHotPartitionType = 0;
    }
    
    return;
}

static void
assign_manual_hot_date(const char *newval, void *extra)
{// #lizard forgives
    fsec_t fsec;
    Datum stamp;
    int32  gap;
    
    if (newval && newval[0] != '\0')
    {
        stamp = DirectFunctionCall3(timestamp_in,
                                    CStringGetDatum(newval),
                                    ObjectIdGetDatum(InvalidOid),
                                    Int32GetDatum(-1));
        
        if(timestamp2tm(stamp, NULL, &g_ManualHotDataTime, &fsec, NULL, NULL) != 0)
        {
            ereport(ERROR,
                        (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                         errmsg("timestamp out of range")));    
        }

        if (g_ColdHotPartitionType == IntervalType_Day)
        {
            g_ManualHotDataTime.tm_sec = 0;
            g_ManualHotDataTime.tm_min = 0;
            g_ManualHotDataTime.tm_hour = 0;
        }
        else if (g_ColdHotPartitionType == IntervalType_Month)
        {
            g_ManualHotDataTime.tm_sec = 0;
            g_ManualHotDataTime.tm_min = 0;
            g_ManualHotDataTime.tm_hour = 0;
            g_ManualHotDataTime.tm_mday = 1;
        }
        else if (g_ColdHotPartitionType == IntervalType_Year)
        {
            g_ManualHotDataTime.tm_sec = 0;
            g_ManualHotDataTime.tm_min = 0;
            g_ManualHotDataTime.tm_hour = 0;
            g_ManualHotDataTime.tm_mday = 1;
            g_ManualHotDataTime.tm_mon = 1;
        }
        else
        {
            ereport(ERROR,
                    (errcode(ERRCODE_UNDEFINED_PARAMETER),
                     errmsg("cold_hot_sepration_mode must be set before"))); 
        }

        g_ManualHotDataGapWithMonths = get_months_away_from_base(&g_ManualHotDataTime);
                                                         
        g_ManualHotDataGapWithDays   = get_days_away_from_base(&g_ManualHotDataTime);

        gap = date_diff_indays(&g_ManualHotDataTime);
        if (gap < 0)
        {
            memset((char*)&g_ManualHotDataTime, 0x0, sizeof(g_ManualHotDataTime));
            g_ManualHotDataGapWithDays   = 0;
            g_ManualHotDataGapWithMonths = 0;
            ereport(ERROR,
                        (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                         errmsg("Invalid manual_hot_date, %d days away from base, hot_data_age:%d", gap, g_ColdDataThreashold)));    
        }
    }  
    else
    {
        memset((char*)&g_ManualHotDataTime, 0x0, sizeof(g_ManualHotDataTime));
        g_ManualHotDataGapWithDays   = 0;
        g_ManualHotDataGapWithMonths = 0;
    }
    
    return;
}

static void
assign_template_cold_date(const char *newval, void *extra)
{
    fsec_t fsec;
    Datum stamp;
    if (newval && newval[0] != '\0')
    {
        stamp = DirectFunctionCall3(timestamp_in,
                                    CStringGetDatum(newval),
                                    ObjectIdGetDatum(InvalidOid),
                                    Int32GetDatum(-1));
        
        if(timestamp2tm(stamp, NULL, &g_TempColdDataTime, &fsec, NULL, NULL) != 0)
        {
            ereport(ERROR,
                        (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                         errmsg("timestamp out of range")));    
        }
    }    
}

static void
assign_template_hot_date(const char *newval, void *extra)
{
    fsec_t fsec;
    Datum stamp;
    if (newval && newval[0] != '\0')
    {
        stamp = DirectFunctionCall3(timestamp_in,
                                    CStringGetDatum(newval),
                                    ObjectIdGetDatum(InvalidOid),
                                    Int32GetDatum(-1));
        
        if(timestamp2tm(stamp, NULL, &g_TempHotDataTime, &fsec, NULL, NULL) != 0)
        {
            ereport(ERROR,
                        (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                         errmsg("timestamp out of range")));    
        }

        if (g_ColdHotPartitionType == IntervalType_Day)
        {
            g_TempHotDataTime.tm_sec = 0;
            g_TempHotDataTime.tm_min = 0;
            g_TempHotDataTime.tm_hour = 0;
        }
        else if (g_ColdHotPartitionType == IntervalType_Month)
        {
            g_TempHotDataTime.tm_sec = 0;
            g_TempHotDataTime.tm_min = 0;
            g_TempHotDataTime.tm_hour = 0;
            g_TempHotDataTime.tm_mday = 1;
        }
        else if (g_ColdHotPartitionType == IntervalType_Year)
        {
            g_TempHotDataTime.tm_sec = 0;
            g_TempHotDataTime.tm_min = 0;
            g_TempHotDataTime.tm_hour = 0;
            g_TempHotDataTime.tm_mday = 1;
            g_TempHotDataTime.tm_mon = 1;
        }
        else
        {
            ereport(ERROR,
                    (errcode(ERRCODE_UNDEFINED_PARAMETER),
                     errmsg("cold_hot_sepration_mode must be set before"))); 
        }
    }    
    
    return;
}

#endif
#ifdef _MLS_
static void assign_default_locator_type(const char *newval, void *extra)
{
    if (newval && newval[0] != '\0')
    {
        if ((strcmp(newval, "hash") != 0) 
            && (strcmp(newval, "shard") != 0) 
            && (strcmp(newval, "replication") != 0))
        {
            elog(ERROR, "invalid for default_locator_type, enum as shard/hash/replication");
        }
        
        g_default_locator_type = strdup(newval);
    }    
    
    return;
}

#endif

#ifdef _PUB_SUB_RELIABLE_
static void assign_wal_stream_type(const char *newval, void *extra)
{
    if (newval && newval[0] != '\0')
    {
        if (strcmp(newval, "user_stream") == 0)
        {
            wal_reset_stream();
        }
        else if (strcmp(newval, "cluster_stream") == 0) 
        {
            wal_set_cluster_stream();
        }
        else if (strcmp(newval, "internal_stream") == 0)
        {
            wal_set_internal_stream();   
        }
        else
        {
            elog(ERROR, "invalid for wal_stream_type, enum as user_stream/cluster_stream/internal_stream");
        }
        
        g_wal_stream_type_str = strdup(newval);
    }    
    
    return;
}
#endif


#include "guc-file.c"
