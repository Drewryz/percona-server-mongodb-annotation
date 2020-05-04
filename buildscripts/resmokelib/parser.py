"""Parser for command line arguments."""

import os
import sys

import argparse

from . import config as _config
from . import configure_resmoke
from . import commands

_EVERGREEN_ARGUMENT_TITLE = "Evergreen options"


def _make_parser():
    """Create and return the command line arguments parser."""
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command")

    # Add sub-commands.
    _add_run(subparsers)
    _add_list_suites(subparsers)
    _add_find_suites(subparsers)
    _add_hang_analyzer(subparsers)

    return parser


def _add_run(subparsers):  # pylint: disable=too-many-statements
    """Create and add the parser for the Run subcommand."""
    parser = subparsers.add_parser("run", help="Runs the specified tests.")

    parser.set_defaults(dry_run="off", shuffle="auto", stagger_jobs="off",
                        suite_files="with_server", majority_read_concern="on")

    parser.add_argument("test_files", metavar="TEST_FILES", nargs="*",
                        help="Explicit test files to run")

    parser.add_argument(
        "--suites", dest="suite_files", metavar="SUITE1,SUITE2",
        help=("Comma separated list of YAML files that each specify the configuration"
              " of a suite. If the file is located in the resmokeconfig/suites/"
              " directory, then the basename without the .yml extension can be"
              " specified, e.g. 'core'. If a list of files is passed in as"
              " positional arguments, they will be run using the suites'"
              " configurations."))

    parser.add_argument(
        "--log", dest="logger_file", metavar="LOGGER",
        help=("A YAML file that specifies the logging configuration. If the file is"
              " located in the resmokeconfig/suites/ directory, then the basename"
              " without the .yml extension can be specified, e.g. 'console'."))
    parser.set_defaults(logger_file="console")

    parser.add_argument(
        "--archiveFile", dest="archive_file", metavar="ARCHIVE_FILE",
        help=("Sets the archive file name for the Evergreen task running the tests."
              " The archive file is JSON format containing a list of tests that were"
              " successfully archived to S3. If unspecified, no data files from tests"
              " will be archived in S3. Tests can be designated for archival in the"
              " task suite configuration file."))

    parser.add_argument(
        "--archiveLimitMb", type=int, dest="archive_limit_mb", metavar="ARCHIVE_LIMIT_MB",
        help=("Sets the limit (in MB) for archived files to S3. A value of 0"
              " indicates there is no limit."))

    parser.add_argument(
        "--archiveLimitTests", type=int, dest="archive_limit_tests", metavar="ARCHIVE_LIMIT_TESTS",
        help=("Sets the maximum number of tests to archive to S3. A value"
              " of 0 indicates there is no limit."))

    parser.add_argument(
        "--basePort", dest="base_port", metavar="PORT",
        help=("The starting port number to use for mongod and mongos processes"
              " spawned by resmoke.py or the tests themselves. Each fixture and Job"
              " allocates a contiguous range of ports."))

    parser.add_argument("--buildloggerUrl", action="store", dest="buildlogger_url", metavar="URL",
                        help="The root url of the buildlogger server.")

    parser.add_argument("--continueOnFailure", action="store_true", dest="continue_on_failure",
                        help="Executes all tests in all suites, even if some of them fail.")

    parser.add_argument(
        "--dbpathPrefix", dest="dbpath_prefix", metavar="PATH",
        help=("The directory which will contain the dbpaths of any mongod's started"
              " by resmoke.py or the tests themselves."))

    parser.add_argument("--dbtest", dest="dbtest_executable", metavar="PATH",
                        help="The path to the dbtest executable for resmoke to use.")

    parser.add_argument(
        "--excludeWithAnyTags", action="append", dest="exclude_with_any_tags", metavar="TAG1,TAG2",
        help=("Comma separated list of tags. Any jstest that contains any of the"
              " specified tags will be excluded from any suites that are run."
              " The tag '{}' is implicitly part of this list.".format(_config.EXCLUDED_TAG)))

    parser.add_argument("--genny", dest="genny_executable", metavar="PATH",
                        help="The path to the genny executable for resmoke to use.")

    parser.add_argument(
        "--spawnUsing", dest="spawn_using", choices=("python", "jasper"),
        help=("Allows you to spawn resmoke processes using python or Jasper."
              "Defaults to python. Options are 'python' or 'jasper'."))

    parser.add_argument(
        "--includeWithAnyTags", action="append", dest="include_with_any_tags", metavar="TAG1,TAG2",
        help=("Comma separated list of tags. For the jstest portion of the suite(s),"
              " only tests which have at least one of the specified tags will be"
              " run."))

    parser.add_argument("-n", action="store_const", const="tests", dest="dry_run",
                        help="Outputs the tests that would be run.")

    # TODO: add support for --dryRun=commands
    parser.add_argument(
        "--dryRun", action="store", dest="dry_run", choices=("off", "tests"), metavar="MODE",
        help=("Instead of running the tests, outputs the tests that would be run"
              " (if MODE=tests). Defaults to MODE=%%default."))

    parser.add_argument(
        "-j", "--jobs", type=int, dest="jobs", metavar="JOBS",
        help=("The number of Job instances to use. Each instance will receive its"
              " own MongoDB deployment to dispatch tests to."))

    parser.add_argument("--mongo", dest="mongo_executable", metavar="PATH",
                        help="The path to the mongo shell executable for resmoke.py to use.")

    parser.add_argument("--mongod", dest="mongod_executable", metavar="PATH",
                        help="The path to the mongod executable for resmoke.py to use.")

    parser.add_argument(
        "--mongodSetParameters", dest="mongod_set_parameters",
        metavar="{key1: value1, key2: value2, ..., keyN: valueN}",
        help=("Passes one or more --setParameter options to all mongod processes"
              " started by resmoke.py. The argument is specified as bracketed YAML -"
              " i.e. JSON with support for single quoted and unquoted keys."))

    parser.add_argument("--mongos", dest="mongos_executable", metavar="PATH",
                        help="The path to the mongos executable for resmoke.py to use.")

    parser.add_argument(
        "--mongosSetParameters", dest="mongos_set_parameters",
        metavar="{key1: value1, key2: value2, ..., keyN: valueN}",
        help=("Passes one or more --setParameter options to all mongos processes"
              " started by resmoke.py. The argument is specified as bracketed YAML -"
              " i.e. JSON with support for single quoted and unquoted keys."))

    parser.add_argument("--nojournal", action="store_true", dest="no_journal",
                        help="Disables journaling for all mongod's.")

    parser.add_argument("--numClientsPerFixture", type=int, dest="num_clients_per_fixture",
                        help="Number of clients running tests per fixture.")

    parser.add_argument("--perfReportFile", dest="perf_report_file", metavar="PERF_REPORT",
                        help="Writes a JSON file with performance test results.")

    parser.add_argument(
        "--shellConnString", dest="shell_conn_string", metavar="CONN_STRING",
        help="Overrides the default fixture and connects with a mongodb:// connection"
        " string to an existing MongoDB cluster instead. This is useful for"
        " connecting to a MongoDB deployment started outside of resmoke.py including"
        " one running in a debugger.")

    parser.add_argument(
        "--shellPort", dest="shell_port", metavar="PORT",
        help="Convenience form of --shellConnString for connecting to an"
        " existing MongoDB cluster with the URL mongodb://localhost:[PORT]."
        " This is useful for connecting to a server running in a debugger.")

    parser.add_argument("--repeat", "--repeatSuites", type=int, dest="repeat_suites", metavar="N",
                        help="Repeats the given suite(s) N times, or until one fails.")

    parser.add_argument(
        "--repeatTests", type=int, dest="repeat_tests", metavar="N",
        help="Repeats the tests inside each suite N times. This applies to tests"
        " defined in the suite configuration as well as tests defined on the command"
        " line.")

    parser.add_argument(
        "--repeatTestsMax", type=int, dest="repeat_tests_max", metavar="N",
        help="Repeats the tests inside each suite no more than N time when"
        " --repeatTestsSecs is specified. This applies to tests defined in the suite"
        " configuration as well as tests defined on the command line.")

    parser.add_argument(
        "--repeatTestsMin", type=int, dest="repeat_tests_min", metavar="N",
        help="Repeats the tests inside each suite at least N times when"
        " --repeatTestsSecs is specified. This applies to tests defined in the suite"
        " configuration as well as tests defined on the command line.")

    parser.add_argument(
        "--repeatTestsSecs", type=float, dest="repeat_tests_secs", metavar="SECONDS",
        help="Repeats the tests inside each suite this amount of time. Note that"
        " this option is mutually exclusive with --repeatTests. This applies to"
        " tests defined in the suite configuration as well as tests defined on the"
        " command line.")

    parser.add_argument(
        "--reportFailureStatus", action="store", dest="report_failure_status",
        choices=("fail", "silentfail"), metavar="STATUS",
        help="Controls if the test failure status should be reported as failed"
        " or be silently ignored (STATUS=silentfail). Dynamic test failures will"
        " never be silently ignored. Defaults to STATUS=%%default.")

    parser.add_argument("--reportFile", dest="report_file", metavar="REPORT",
                        help="Writes a JSON file with test status and timing information.")

    parser.add_argument(
        "--seed", type=int, dest="seed", metavar="SEED",
        help=("Seed for the random number generator. Useful in combination with the"
              " --shuffle option for producing a consistent test execution order."))

    parser.add_argument("--serviceExecutor", dest="service_executor", metavar="EXECUTOR",
                        help="The service executor used by jstests")

    parser.add_argument("--transportLayer", dest="transport_layer", metavar="TRANSPORT",
                        help="The transport layer used by jstests")

    parser.add_argument("--shellReadMode", action="store", dest="shell_read_mode",
                        choices=("commands", "compatibility", "legacy"), metavar="READ_MODE",
                        help="The read mode used by the mongo shell.")

    parser.add_argument("--shellWriteMode", action="store", dest="shell_write_mode",
                        choices=("commands", "compatibility", "legacy"), metavar="WRITE_MODE",
                        help="The write mode used by the mongo shell.")

    parser.add_argument(
        "--shuffle", action="store_const", const="on", dest="shuffle",
        help=("Randomizes the order in which tests are executed. This is equivalent"
              " to specifying --shuffleMode=on."))

    parser.add_argument(
        "--shuffleMode", action="store", dest="shuffle", choices=("on", "off",
                                                                  "auto"), metavar="ON|OFF|AUTO",
        help=("Controls whether to randomize the order in which tests are executed."
              " Defaults to auto when not supplied. auto enables randomization in"
              " all cases except when the number of jobs requested is 1."))

    parser.add_argument(
        "--staggerJobs", action="store", dest="stagger_jobs", choices=("on", "off"),
        metavar="ON|OFF", help=("Enables or disables the stagger of launching resmoke jobs."
                                " Defaults to %%default."))

    parser.add_argument(
        "--majorityReadConcern", action="store", dest="majority_read_concern", choices=("on",
                                                                                        "off"),
        metavar="ON|OFF", help=("Enable or disable majority read concern support."
                                " Defaults to %%default."))

    parser.add_argument("--flowControl", action="store", dest="flow_control", choices=("on", "off"),
                        metavar="ON|OFF", help=("Enable or disable flow control."))

    parser.add_argument("--flowControlTicketOverride", type=int, action="store",
                        dest="flow_control_tickets", metavar="TICKET_OVERRIDE",
                        help=("Number of tickets available for flow control."))

    parser.add_argument("--storageEngine", dest="storage_engine", metavar="ENGINE",
                        help="The storage engine used by dbtests and jstests.")

    parser.add_argument(
        "--storageEngineCacheSizeGB", dest="storage_engine_cache_size_gb", metavar="CONFIG",
        help="Sets the storage engine cache size configuration"
        " setting for all mongod's.")

    parser.add_argument("--tagFile", dest="tag_file", metavar="OPTIONS",
                        help="A YAML file that associates tests and tags.")

    parser.add_argument(
        "--wiredTigerCollectionConfigString", dest="wt_coll_config", metavar="CONFIG",
        help="Sets the WiredTiger collection configuration setting for all mongod's.")

    parser.add_argument("--wiredTigerEngineConfigString", dest="wt_engine_config", metavar="CONFIG",
                        help="Sets the WiredTiger engine configuration setting for all mongod's.")

    parser.add_argument("--wiredTigerIndexConfigString", dest="wt_index_config", metavar="CONFIG",
                        help="Sets the WiredTiger index configuration setting for all mongod's.")

    parser.add_argument(
        "--executor", dest="executor_file",
        help="OBSOLETE: Superceded by --suites; specify --suites=SUITE path/to/test"
        " to run a particular test under a particular suite configuration.")

    evergreen_options = parser.add_argument_group(
        title=_EVERGREEN_ARGUMENT_TITLE,
        description=("Options used to propagate information about the Evergreen task running this"
                     " script."))

    evergreen_options.add_argument("--buildId", dest="build_id", metavar="BUILD_ID",
                                   help="Sets the build ID of the task.")

    evergreen_options.add_argument(
        "--distroId", dest="distro_id", metavar="DISTRO_ID",
        help=("Sets the identifier for the Evergreen distro running the"
              " tests."))

    evergreen_options.add_argument(
        "--executionNumber", type=int, dest="execution_number", metavar="EXECUTION_NUMBER",
        help=("Sets the number for the Evergreen execution running the"
              " tests."))

    evergreen_options.add_argument(
        "--gitRevision", dest="git_revision", metavar="GIT_REVISION",
        help=("Sets the git revision for the Evergreen task running the"
              " tests."))

    # We intentionally avoid adding a new command line option that starts with --suite so it doesn't
    # become ambiguous with the --suites option and break how engineers run resmoke.py locally.
    evergreen_options.add_argument(
        "--originSuite", dest="origin_suite", metavar="SUITE",
        help=("Indicates the name of the test suite prior to the"
              " evergreen_generate_resmoke_tasks.py script splitting it"
              " up."))

    evergreen_options.add_argument(
        "--patchBuild", action="store_true", dest="patch_build",
        help=("Indicates that the Evergreen task running the tests is a"
              " patch build."))

    evergreen_options.add_argument(
        "--projectName", dest="project_name", metavar="PROJECT_NAME",
        help=("Sets the name of the Evergreen project running the tests."))

    evergreen_options.add_argument("--revisionOrderId", dest="revision_order_id",
                                   metavar="REVISION_ORDER_ID",
                                   help="Sets the chronological order number of this commit.")

    evergreen_options.add_argument("--taskName", dest="task_name", metavar="TASK_NAME",
                                   help="Sets the name of the Evergreen task running the tests.")

    evergreen_options.add_argument("--taskId", dest="task_id", metavar="TASK_ID",
                                   help="Sets the Id of the Evergreen task running the tests.")

    evergreen_options.add_argument(
        "--variantName", dest="variant_name", metavar="VARIANT_NAME",
        help=("Sets the name of the Evergreen build variant running the"
              " tests."))

    evergreen_options.add_argument("--versionId", dest="version_id", metavar="VERSION_ID",
                                   help="Sets the version ID of the task.")

    benchmark_options = parser.add_argument_group(
        title="Benchmark/Benchrun test options",
        description="Options for running Benchmark/Benchrun tests")

    benchmark_options.add_argument("--benchmarkFilter", type=str, dest="benchmark_filter",
                                   metavar="BENCHMARK_FILTER",
                                   help="Regex to filter Google benchmark tests to run.")

    benchmark_options.add_argument(
        "--benchmarkListTests",
        dest="benchmark_list_tests",
        action="store_true",
        # metavar="BENCHMARK_LIST_TESTS",
        help=("Lists all Google benchmark test configurations in each"
              " test file."))

    benchmark_min_time_help = (
        "Minimum time to run each benchmark/benchrun test for. Use this option instead of "
        "--benchmarkRepetitions to make a test run for a longer or shorter duration.")
    benchmark_options.add_argument("--benchmarkMinTimeSecs", type=int,
                                   dest="benchmark_min_time_secs", metavar="BENCHMARK_MIN_TIME",
                                   help=benchmark_min_time_help)

    benchmark_repetitions_help = (
        "Set --benchmarkRepetitions=1 if you'd like to run the benchmark/benchrun tests only once."
        " By default, each test is run multiple times to provide statistics on the variance"
        " between runs; use --benchmarkMinTimeSecs if you'd like to run a test for a longer or"
        " shorter duration.")
    benchmark_options.add_argument("--benchmarkRepetitions", type=int, dest="benchmark_repetitions",
                                   metavar="BENCHMARK_REPETITIONS", help=benchmark_repetitions_help)


def _add_list_suites(subparsers):
    """Create and add the parser for the list-suites subcommand."""
    parser = subparsers.add_parser("list-suites",
                                   help="Lists the names of the suites available to execute.")

    parser.add_argument(
        "--log", dest="logger_file", metavar="LOGGER",
        help=("A YAML file that specifies the logging configuration. If the file is"
              " located in the resmokeconfig/suites/ directory, then the basename"
              " without the .yml extension can be specified, e.g. 'console'."))
    parser.set_defaults(logger_file="console")


def _add_find_suites(subparsers):
    """Create and add the parser for the find-suites subcommand."""
    parser = subparsers.add_parser(
        "find-suites", help="Lists the names of the suites that will execute the specified tests.")

    parser.add_argument("test_files", metavar="TEST_FILES", nargs="*",
                        help="Explicit test files to run")

    parser.add_argument(
        "--log", dest="logger_file", metavar="LOGGER",
        help=("A YAML file that specifies the logging configuration. If the file is"
              " located in the resmokeconfig/suites/ directory, then the basename"
              " without the .yml extension can be specified, e.g. 'console'."))
    parser.set_defaults(logger_file="console")

    parser.add_argument(
        "--suites", dest="suite_files", metavar="SUITE1,SUITE2", required=True,
        help=("Comma separated list of YAML files that each specify the configuration"
              " of a suite. If the file is located in the resmokeconfig/suites/"
              " directory, then the basename without the .yml extension can be"
              " specified, e.g. 'core'. If a list of files is passed in as"
              " positional arguments, they will be run using the suites'"
              " configurations."))


def _add_hang_analyzer(subparsers):
    """Create and add the parser for the hang analyzer subcommand."""

    parser = subparsers.add_parser("hang-analyzer", help=commands.hang_analyzer.__doc__)

    parser.add_argument(
        '-m', '--process-match', dest='process_match', choices=('contains', 'exact'),
        default='contains', help="Type of match for process names (-p & -g), specify 'contains', or"
        " 'exact'. Note that the process name match performs the following"
        " conversions: change all process names to lowecase, strip off the file"
        " extension, like '.exe' on Windows. Default is 'contains'.")
    parser.add_argument('-p', '--process-names', dest='process_names',
                        help='Comma separated list of process names to analyze')
    parser.add_argument('-g', '--go-process-names', dest='go_process_names',
                        help='Comma separated list of go process names to analyze')
    parser.add_argument(
        '-d', '--process-ids', dest='process_ids', default=None,
        help='Comma separated list of process ids (PID) to analyze, overrides -p &'
        ' -g')
    parser.add_argument('-c', '--dump-core', dest='dump_core', action="store_true", default=False,
                        help='Dump core file for each analyzed process')
    parser.add_argument('-s', '--max-core-dumps-size', dest='max_core_dumps_size', default=10000,
                        help='Maximum total size of core dumps to keep in megabytes')
    parser.add_argument(
        '-o', '--debugger-output', dest='debugger_output', action="append", choices=('file',
                                                                                     'stdout'),
        default=None, help="If 'stdout', then the debugger's output is written to the Python"
        " process's stdout. If 'file', then the debugger's output is written"
        " to a file named debugger_<process>_<pid>.log for each process it"
        " attaches to. This option can be specified multiple times on the"
        " command line to have the debugger's output written to multiple"
        " locations. By default, the debugger's output is written only to the"
        " Python process's stdout.")


# def to_local_args(args=None):  # pylint: disable=too-many-branches,too-many-locals
#     """
#     Return a command line invocation for resmoke.py suitable for being run outside of Evergreen.
#     This function parses the 'args' list of command line arguments, removes any Evergreen-centric
#     options, and returns a new list of command line arguments.
#     """

#     if args is None:
#         args = sys.argv[1:]

#     parser = _make_parser()

#     # We call optparse.OptionParser.parse_args() with a new instance of optparse.Values to avoid
#     # having the default values filled in. This makes it so 'options' only contains command line
#     # options that were explicitly specified.
#     options, extra_args = parser.parse_args(args=args, values=optparse.Values())

#     # If --originSuite was specified, then we replace the value of --suites with it. This is done to
#     # avoid needing to have engineers learn about the test suites generated by the
#     # evergreen_generate_resmoke_tasks.py script.
#     origin_suite = getattr(options, "origin_suite", None)
#     if origin_suite is not None:
#         setattr(options, "suite_files", origin_suite)

#     # optparse.OptionParser doesn't offer a public and/or documented method for getting all of the
#     # options. Given that the optparse module is deprecated, it is unlikely for the
#     # _get_all_options() method to ever be removed or renamed.
#     all_options = parser._get_all_options()  # pylint: disable=protected-access

#     options_by_dest = {}
#     for option in all_options:
#         options_by_dest[option.dest] = option

#     suites_arg = None
#     storage_engine_arg = None
#     other_local_args = []

#     options_to_ignore = {
#         "--archiveLimitMb",
#         "--archiveLimitTests",
#         "--buildloggerUrl",
#         "--log",
#         "--perfReportFile",
#         "--reportFailureStatus",
#         "--reportFile",
#         "--staggerJobs",
#         "--tagFile",
#     }

#     def format_option(option_name, option_value):
#         """
#         Return <option_name>=<option_value>.
#         This function assumes that 'option_name' is always "--" prefix and isn't "-" prefixed.
#         """
#         return "%s=%s" % (option_name, option_value)

#     for option_dest in sorted(vars(options)):
#         option_value = getattr(options, option_dest)
#         option = options_by_dest[option_dest]
#         option_name = option.get_opt_string()

#         if option_name in options_to_ignore:
#             continue

#         option_group = parser.get_option_group(option_name)
#         if option_group is not None and option_group.title == _EVERGREEN_OPTIONS_TITLE:
#             continue

#         if option.takes_value():
#             if option.action == "append":
#                 args = [format_option(option_name, elem) for elem in option_value]
#                 other_local_args.extend(args)
#             else:
#                 arg = format_option(option_name, option_value)

#                 # We track the value for the --suites and --storageEngine command line options
#                 # separately in order to more easily sort them to the front.
#                 if option_dest == "suite_files":
#                     suites_arg = arg
#                 elif option_dest == "storage_engine":
#                     storage_engine_arg = arg
#                 else:
#                     other_local_args.append(arg)
#         else:
#             other_local_args.append(option_name)

#     return [arg for arg in (suites_arg, storage_engine_arg) if arg is not None
#             ] + other_local_args + extra_args


def _parse(sys_args):
    """Parse the CLI args."""

    # Split out this function for easier testing.
    parser = _make_parser()
    parsed_args = parser.parse_args(sys_args)

    return (parser, parsed_args)


def parse_command_line(sys_args, **kwargs):
    """Parse the command line arguments passed to resmoke.py and return the subcommand object to execute."""
    parser, parsed_args = _parse(sys_args)

    subcommand = parsed_args.command
    subcommand_obj = None
    if subcommand in ('find-suites', 'list-suites', 'run'):
        configure_resmoke.validate_and_update_config(parser, parsed_args)
        if _config.EVERGREEN_TASK_ID is not None:
            subcommand_obj = commands.run.TestRunnerEvg(subcommand, **kwargs)
        else:
            subcommand_obj = commands.run.TestRunner(subcommand, **kwargs)
    elif subcommand == 'hang-analyzer':
        subcommand_obj = commands.hang_analyzer.HangAnalyzer(parsed_args)

    if subcommand_obj is None:
        raise RuntimeError(
            f"Resmoke configuration has invalid subcommand: {subcommand}. Try '--help'")

    return subcommand_obj
