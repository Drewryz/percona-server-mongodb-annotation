#!/usr/bin/env python3
"""
Resmoke Test Suite Generator.

Analyze the evergreen history for tests run under the given task and create new evergreen tasks
to attempt to keep the task runtime under a specified amount.
"""
from copy import deepcopy
import datetime
from datetime import timedelta
import logging
import math
import os
import re
import sys
from distutils.util import strtobool  # pylint: disable=no-name-in-module
from typing import Dict, List, Tuple

import click
import requests
import structlog
import yaml

from evergreen.api import RetryingEvergreenApi
from shrub.config import Configuration
from shrub.task import TaskDependency
from shrub.variant import DisplayTaskDefinition
from shrub.variant import TaskSpec

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import buildscripts.resmokelib.parser as _parser  # pylint: disable=wrong-import-position
import buildscripts.resmokelib.suitesconfig as suitesconfig  # pylint: disable=wrong-import-position
import buildscripts.util.read_config as read_config  # pylint: disable=wrong-import-position
import buildscripts.util.taskname as taskname  # pylint: disable=wrong-import-position
import buildscripts.util.teststats as teststats  # pylint: disable=wrong-import-position

# pylint: disable=wrong-import-position
from buildscripts.patch_builds.task_generation import TimeoutInfo, resmoke_commands
# pylint: enable=wrong-import-position

LOGGER = structlog.getLogger(__name__)

DEFAULT_TEST_SUITE_DIR = os.path.join("buildscripts", "resmokeconfig", "suites")
CONFIG_FILE = "./.evergreen.yml"
MIN_TIMEOUT_SECONDS = int(timedelta(minutes=5).total_seconds())
LOOKBACK_DURATION_DAYS = 14
GEN_SUFFIX = "_gen"

HEADER_TEMPLATE = """# DO NOT EDIT THIS FILE. All manual edits will be lost.
# This file was generated by {file} from
# {suite_file}.
"""

REQUIRED_CONFIG_KEYS = {
    "build_variant",
    "fallback_num_sub_suites",
    "project",
    "task_id",
    "task_name",
}

DEFAULT_CONFIG_VALUES = {
    "generated_config_dir": "generated_resmoke_config",
    "max_tests_per_suite": 100,
    "resmoke_args": "",
    "resmoke_repeat_suites": 1,
    "run_multiple_jobs": "true",
    "target_resmoke_time": 60,
    "test_suites_dir": DEFAULT_TEST_SUITE_DIR,
    "use_default_timeouts": False,
    "use_large_distro": False,
}

CONFIG_FORMAT_FN = {
    "fallback_num_sub_suites": int,
    "max_sub_suites": int,
    "max_tests_per_suite": int,
    "target_resmoke_time": int,
}


class ConfigOptions(object):
    """Retrieve configuration from a config file."""

    def __init__(self, config, required_keys=None, defaults=None, formats=None):
        """
        Create an instance of ConfigOptions.

        :param config: Dictionary of configuration to use.
        :param required_keys: Set of keys required by this config.
        :param defaults: Dict of default values for keys.
        :param formats: Dict with functions to format values before returning.
        """
        self.config = config
        self.required_keys = required_keys if required_keys else set()
        self.default_values = defaults if defaults else {}
        self.formats = formats if formats else {}

    @classmethod
    def from_file(cls, filepath, required_keys, defaults, formats):
        """
        Create an instance of ConfigOptions based on the given config file.

        :param filepath: Path to file containing configuration.
        :param required_keys: Set of keys required by this config.
        :param defaults: Dict of default values for keys.
        :param formats: Dict with functions to format values before returning.
        :return: Instance of ConfigOptions.
        """
        return cls(read_config.read_config_file(filepath), required_keys, defaults, formats)

    @property
    def depends_on(self):
        """List of dependencies specified."""
        return split_if_exists(self._lookup(self.config, "depends_on"))

    @property
    def is_patch(self):
        """Is this running in a patch build."""
        patch = self.config.get("is_patch")
        if patch:
            return strtobool(patch)
        return None

    @property
    def repeat_suites(self):
        """How many times should the suite be repeated."""
        return int(self.resmoke_repeat_suites)

    @property
    def suite(self):
        """Return test suite is being run."""
        return self.config.get("suite", self.task)

    @property
    def task(self):
        """Return task being run."""
        return remove_gen_suffix(self.task_name)

    @property
    def variant(self):
        """Return build variant is being run on."""
        return self.build_variant

    def _lookup(self, config, item):
        if item not in config:
            if item in self.required_keys:
                raise KeyError(f"{item} must be specified in configuration.")
            return self.default_values.get(item, None)

        if item in self.formats and item in config:
            return self.formats[item](config[item])

        return config.get(item, None)

    def __getattr__(self, item):
        """Determine the value of the given attribute."""
        return self._lookup(self.config, item)

    def __repr__(self):
        """Provide a string representation of this object for debugging."""
        required_values = [f"{key}: {self.config[key]}" for key in REQUIRED_CONFIG_KEYS]
        return f"ConfigOptions({', '.join(required_values)})"


def enable_logging(verbose):
    """Enable verbose logging for execution."""

    level = logging.DEBUG if verbose else logging.INFO
    logging.basicConfig(
        format="[%(asctime)s - %(name)s - %(levelname)s] %(message)s",
        level=level,
        stream=sys.stdout,
    )
    structlog.configure(logger_factory=structlog.stdlib.LoggerFactory())


def write_file(directory: str, filename: str, contents: str):
    """
    Write the given contents to the specified file.

    :param directory: Directory to write file into.
    :param filename: Name of file to write to.
    :param contents: Data to write to file.
    """
    with open(os.path.join(directory, filename), "w") as fileh:
        fileh.write(contents)


def write_file_dict(directory: str, file_dict: Dict[str, str]):
    """
    Write files in the given dictionary to disk.

    The keys of the dictionary should be the filenames to write and the values should be
    the contents to write to each file.

    If the given directory does not exist, it will be created.

    :param directory: Directory to write files to.
    :param file_dict: Dictionary of files to write.
    """
    if not os.path.exists(directory):
        os.makedirs(directory)

    for name, contents in file_dict.items():
        write_file(directory, name, contents)


def read_yaml(directory: str, filename: str) -> Dict:
    """
    Read the given yaml file.

    :param directory: Directory containing file.
    :param filename: Name of file to read.
    :return: Yaml contents of file.
    """
    with open(os.path.join(directory, filename), "r") as fileh:
        return yaml.safe_load(fileh)


def split_if_exists(str_to_split):
    """Split the given string on "," if it is not None."""
    if str_to_split:
        return str_to_split.split(",")
    return None


def remove_gen_suffix(task_name):
    """Remove '_gen' suffix from task_name."""
    if task_name.endswith(GEN_SUFFIX):
        return task_name[:-4]
    return task_name


def string_contains_any_of_args(string, args):
    """
    Return whether array contains any of a group of args.

    :param string: String being checked.
    :param args: Args being analyzed.
    :return: True if any args are found in the string.
    """
    return any(arg in string for arg in args)


def divide_remaining_tests_among_suites(remaining_tests_runtimes, suites):
    """Divide the list of tests given among the suites given."""
    suite_idx = 0
    for test_file, runtime in remaining_tests_runtimes:
        current_suite = suites[suite_idx]
        current_suite.add_test(test_file, runtime)
        suite_idx += 1
        if suite_idx >= len(suites):
            suite_idx = 0


def _new_suite_needed(current_suite, test_runtime, max_suite_runtime, max_tests_per_suite):
    """
    Check if a new suite should be created for the given suite.

    :param current_suite: Suite currently being added to.
    :param test_runtime: Runtime of test being added.
    :param max_suite_runtime: Max runtime of a single suite.
    :param max_tests_per_suite: Max number of tests in a suite.
    :return: True if a new test suite should be created.
    """
    if current_suite.get_runtime() + test_runtime > max_suite_runtime:
        # Will adding this test put us over the target runtime?
        return True

    if max_tests_per_suite and current_suite.get_test_count() + 1 > max_tests_per_suite:
        # Will adding this test put us over the max number of tests?
        return True

    return False


def divide_tests_into_suites(suite_name, tests_runtimes, max_time_seconds, max_suites=None,
                             max_tests_per_suite=None):
    """
    Divide the given tests into suites.

    Each suite should be able to execute in less than the max time specified. If a single
    test has a runtime greater than `max_time_seconds`, it will be run in a suite on its own.

    If max_suites is reached before assigning all tests to a suite, the remaining tests will be
    divided up among the created suites.

    Note: If `max_suites` is hit, suites may have more tests than `max_tests_per_suite` and may have
    runtimes longer than `max_time_seconds`.

    :param suite_name: Name of suite being split.
    :param tests_runtimes: List of tuples containing test names and test runtimes.
    :param max_time_seconds: Maximum runtime to add to a single bucket.
    :param max_suites: Maximum number of suites to create.
    :param max_tests_per_suite: Maximum number of tests to add to a single suite.
    :return: List of Suite objects representing grouping of tests.
    """
    suites = []
    current_suite = Suite(suite_name)
    last_test_processed = len(tests_runtimes)
    LOGGER.debug("Determines suites for runtime", max_runtime_seconds=max_time_seconds,
                 max_suites=max_suites, max_tests_per_suite=max_tests_per_suite)
    for idx, (test_file, runtime) in enumerate(tests_runtimes):
        LOGGER.debug("Adding test", test=test_file, test_runtime=runtime)
        if _new_suite_needed(current_suite, runtime, max_time_seconds, max_tests_per_suite):
            LOGGER.debug("Finished suite", suite_runtime=current_suite.get_runtime(),
                         test_runtime=runtime, max_time=max_time_seconds)
            if current_suite.get_test_count() > 0:
                suites.append(current_suite)
                current_suite = Suite(suite_name)
                if max_suites and len(suites) >= max_suites:
                    last_test_processed = idx
                    break

        current_suite.add_test(test_file, runtime)

    if current_suite.get_test_count() > 0:
        suites.append(current_suite)

    if max_suites and last_test_processed < len(tests_runtimes):
        # We must have hit the max suite limit, just randomly add the remaining tests to suites.
        divide_remaining_tests_among_suites(tests_runtimes[last_test_processed:], suites)

    return suites


def update_suite_config(suite_config, roots=None, excludes=None):
    """
    Update suite config based on the roots and excludes passed in.

    :param suite_config: suite_config to update.
    :param roots: new roots to run, or None if roots should not be updated.
    :param excludes: excludes to add, or None if excludes should not be include.
    :return: updated suite_config
    """
    if roots:
        suite_config["selector"]["roots"] = roots

    if excludes:
        # This must be a misc file, if the exclude_files section exists, extend it, otherwise,
        # create it.
        if "exclude_files" in suite_config["selector"] and \
                suite_config["selector"]["exclude_files"]:
            suite_config["selector"]["exclude_files"] += excludes
        else:
            suite_config["selector"]["exclude_files"] = excludes
    else:
        # if excludes was not specified this must not a misc file, so don"t exclude anything.
        if "exclude_files" in suite_config["selector"]:
            del suite_config["selector"]["exclude_files"]

    return suite_config


def generate_resmoke_suite_config(source_config, source_file, roots=None, excludes=None):
    """
    Read and evaluate the yaml suite file.

    Override selector.roots and selector.excludes with the provided values. Write the results to
    target_suite_name.

    :param source_config: Config of suite to base generated config on.
    :param source_file: Filename of source suite.
    :param roots: Roots used to select tests for split suite.
    :param excludes: Tests that should be excluded from split suite.
    """
    suite_config = update_suite_config(deepcopy(source_config), roots, excludes)

    contents = HEADER_TEMPLATE.format(file=__file__, suite_file=source_file)
    contents += yaml.safe_dump(suite_config, default_flow_style=False)
    return contents


def render_suite_files(suites: List, suite_name: str, test_list: List[str], suite_dir,
                       update_source_config_cb=None):
    """
    Render the given list of suites.

    This will create a dictionary of all the resmoke config files to create with the
    filename of each file as the key and the contents as the value.

    :param suites: List of suites to render.
    :param suite_name: Base name of suites.
    :param test_list: List of tests used in suites.
    :param suite_dir: Directory containing test suite configurations.
    :param update_source_config_cb: Callback function to update the source_config dictionary.
    :return: Dictionary of rendered resmoke config files.
    """
    source_config = read_yaml(suite_dir, suite_name + ".yml")
    if update_source_config_cb is not None:
        update_source_config_cb(source_config)
    suite_configs = {
        f"{os.path.basename(suite.name)}.yml": suite.generate_resmoke_config(source_config)
        for suite in suites
    }
    suite_configs[f"{os.path.basename(suite_name)}_misc.yml"] = generate_resmoke_suite_config(
        source_config, suite_name, excludes=test_list)
    return suite_configs


def calculate_timeout(avg_runtime, scaling_factor):
    """
    Determine how long a runtime to set based on average runtime and a scaling factor.

    :param avg_runtime: Average runtime of previous runs.
    :param scaling_factor: scaling factor for timeout.
    :return: timeout to use (in seconds).
    """

    def round_to_minute(runtime):
        """Round the given seconds up to the nearest minute."""
        distance_to_min = 60 - (runtime % 60)
        return int(math.ceil(runtime + distance_to_min))

    return max(MIN_TIMEOUT_SECONDS, round_to_minute(avg_runtime)) * scaling_factor


def should_tasks_be_generated(evg_api, task_id):
    """
    Determine if we should attempt to generate tasks.

    If an evergreen task that calls 'generate.tasks' is restarted, the 'generate.tasks' command
    will no-op. So, if we are in that state, we should avoid generating new configuration files
    that will just be confusing to the user (since that would not be used).

    :param evg_api: Evergreen API object.
    :param task_id: Id of the task being run.
    :return: Boolean of whether to generate tasks.
    """
    task = evg_api.task_by_id(task_id, fetch_all_executions=True)
    # If any previous execution was successful, do not generate more tasks.
    for i in range(task.execution):
        task_execution = task.get_execution(i)
        if task_execution.is_success():
            return False

    return True


class EvergreenConfigGenerator(object):
    """Generate evergreen configurations."""

    def __init__(self, suites, options, evg_api):
        """Create new EvergreenConfigGenerator object."""
        self.suites = suites
        self.options = options
        self.evg_api = evg_api
        self.evg_config = Configuration()
        self.task_specs = []
        self.task_names = []
        self.build_tasks = None

    def _set_task_distro(self, task_spec):
        if self.options.use_large_distro and self.options.large_distro_name:
            task_spec.distro(self.options.large_distro_name)

    def _generate_resmoke_args(self, suite_file):
        resmoke_args = "--suite={0}.yml --originSuite={1} {2}".format(
            suite_file, self.options.suite, self.options.resmoke_args)
        if self.options.repeat_suites and not string_contains_any_of_args(
                resmoke_args, ["repeatSuites", "repeat"]):
            resmoke_args += " --repeatSuites={0} ".format(self.options.repeat_suites)

        return resmoke_args

    def _get_run_tests_vars(self, suite_file):
        variables = {
            "resmoke_args": self._generate_resmoke_args(suite_file),
            "run_multiple_jobs": self.options.run_multiple_jobs,
            "task": self.options.task,
        }

        if self.options.resmoke_jobs_max:
            variables["resmoke_jobs_max"] = self.options.resmoke_jobs_max

        if self.options.use_multiversion:
            variables["task_path_suffix"] = self.options.use_multiversion

        return variables

    def _get_timeout_command(self, max_test_runtime, expected_suite_runtime, use_default):
        """
        Add an evergreen command to override the default timeouts to the list of commands.

        :param max_test_runtime: Maximum runtime of any test in the sub-suite.
        :param expected_suite_runtime: Expected runtime of the entire sub-suite.
        :param use_default: Use default timeouts.
        :return: Timeout information.
        """
        repeat_factor = self.options.repeat_suites
        if (max_test_runtime or expected_suite_runtime) and not use_default:
            timeout = None
            exec_timeout = None
            if max_test_runtime:
                timeout = calculate_timeout(max_test_runtime, 3) * repeat_factor
                LOGGER.debug("Setting timeout", timeout=timeout, max_runtime=max_test_runtime,
                             factor=repeat_factor)
            if expected_suite_runtime:
                exec_timeout = calculate_timeout(expected_suite_runtime, 3) * repeat_factor
                LOGGER.debug("Setting exec_timeout", exec_timeout=exec_timeout,
                             suite_runtime=expected_suite_runtime, factor=repeat_factor)
            return TimeoutInfo.overridden(timeout=timeout, exec_timeout=exec_timeout)

        return TimeoutInfo.default_timeout()

    @staticmethod
    def _is_task_dependency(task, possible_dependency):
        return re.match("{0}_(\\d|misc)".format(task), possible_dependency)

    def _get_tasks_for_depends_on(self, dependent_task):
        return [
            str(task.display_name) for task in self.build_tasks
            if self._is_task_dependency(dependent_task, str(task.display_name))
        ]

    def _add_dependencies(self, task):
        task.dependency(TaskDependency("compile"))
        if not self.options.is_patch:
            # Don"t worry about task dependencies in patch builds, only mainline.
            if self.options.depends_on:
                for dep in self.options.depends_on:
                    depends_on_tasks = self._get_tasks_for_depends_on(dep)
                    for dependency in depends_on_tasks:
                        task.dependency(TaskDependency(dependency))

        return task

    def _generate_task(self, sub_suite_name, sub_task_name, target_dir, max_test_runtime=None,
                       expected_suite_runtime=None):
        """Generate evergreen config for a resmoke task."""
        # pylint: disable=too-many-arguments
        LOGGER.debug("Generating task", sub_suite=sub_suite_name)
        spec = TaskSpec(sub_task_name)
        self._set_task_distro(spec)
        self.task_specs.append(spec)

        self.task_names.append(sub_task_name)
        task = self.evg_config.task(sub_task_name)

        target_suite_file = os.path.join(target_dir, os.path.basename(sub_suite_name))
        run_tests_vars = self._get_run_tests_vars(target_suite_file)

        use_multiversion = self.options.use_multiversion
        timeout_info = self._get_timeout_command(max_test_runtime, expected_suite_runtime,
                                                 self.options.use_default_timeouts)
        commands = resmoke_commands("run generated tests", run_tests_vars, timeout_info,
                                    use_multiversion)

        self._add_dependencies(task).commands(commands)

    def _generate_all_tasks(self):
        for idx, suite in enumerate(self.suites):
            sub_task_name = taskname.name_generated_task(self.options.task, idx, len(self.suites),
                                                         self.options.variant)
            max_runtime = None
            total_runtime = None
            if suite.should_overwrite_timeout():
                max_runtime = suite.max_runtime
                total_runtime = suite.get_runtime()
            self._generate_task(suite.name, sub_task_name, self.options.generated_config_dir,
                                max_runtime, total_runtime)

        # Add the misc suite
        misc_suite_name = f"{os.path.basename(self.options.suite)}_misc"
        misc_task_name = f"{self.options.task}_misc_{self.options.variant}"
        self._generate_task(misc_suite_name, misc_task_name, self.options.generated_config_dir)

    def _generate_display_task(self):
        dt = DisplayTaskDefinition(self.options.task)\
            .execution_tasks(self.task_names) \
            .execution_task("{0}_gen".format(self.options.task))
        return dt

    def _generate_variant(self):
        self._generate_all_tasks()

        self.evg_config.variant(self.options.variant)\
            .tasks(self.task_specs)\
            .display_task(self._generate_display_task())

    def generate_config(self):
        """Generate evergreen configuration."""
        self.build_tasks = self.evg_api.tasks_by_build(self.options.build_id)
        self._generate_variant()
        return self.evg_config


class Suite(object):
    """A suite of tests that can be run by evergreen."""

    _current_index = 0

    def __init__(self, source_name: str) -> None:
        """
        Initialize the object.

        :param source_name: Base name of suite.
        """
        self.tests = []
        self.total_runtime = 0
        self.max_runtime = 0
        self.tests_with_runtime_info = 0
        self.source_name = source_name

        self.index = Suite._current_index
        Suite._current_index += 1

    def add_test(self, test_file: str, runtime: float):
        """Add the given test to this suite."""

        self.tests.append(test_file)
        self.total_runtime += runtime

        if runtime != 0:
            self.tests_with_runtime_info += 1

        if runtime > self.max_runtime:
            self.max_runtime = runtime

    def should_overwrite_timeout(self):
        """
        Whether the timeout for this suite should be overwritten.

        We should only overwrite the timeout if we have runtime info for all tests.
        """
        return len(self.tests) == self.tests_with_runtime_info

    def get_runtime(self):
        """Get the current average runtime of all the tests currently in this suite."""

        return self.total_runtime

    def get_test_count(self):
        """Get the number of tests currently in this suite."""

        return len(self.tests)

    @property
    def name(self) -> str:
        """Get the name of this suite."""
        return taskname.name_generated_task(self.source_name, self.index, Suite._current_index)

    def generate_resmoke_config(self, source_config: Dict) -> str:
        """
        Generate the contents of resmoke config for this suite.

        :param source_config: Resmoke config to base generate config on.
        :return: Resmoke config to run this suite.
        """
        suite_config = update_suite_config(deepcopy(source_config), roots=self.tests)
        contents = HEADER_TEMPLATE.format(file=__file__, suite_file=self.source_name)
        contents += yaml.safe_dump(suite_config, default_flow_style=False)
        return contents


class GenerateSubSuites(object):
    """Orchestrate the execution of generate_resmoke_suites."""

    def __init__(self, evergreen_api, config_options):
        """Initialize the object."""
        self.evergreen_api = evergreen_api
        self.config_options = config_options
        self.test_list = []

        # Populate config values for methods like list_tests()
        _parser.set_options()

    def calculate_suites(self, start_date, end_date):
        """Divide tests into suites based on statistics for the provided period."""
        try:
            evg_stats = self.get_evg_stats(self.config_options.project, start_date, end_date,
                                           self.config_options.task, self.config_options.variant)
            if not evg_stats:
                LOGGER.debug("No test history, using fallback suites")
                # This is probably a new suite, since there is no test history, just use the
                # fallback values.
                return self.calculate_fallback_suites()
            target_execution_time_secs = self.config_options.target_resmoke_time * 60
            return self.calculate_suites_from_evg_stats(evg_stats, target_execution_time_secs)
        except requests.HTTPError as err:
            if err.response.status_code == requests.codes.SERVICE_UNAVAILABLE:
                # Evergreen may return a 503 when the service is degraded.
                # We fall back to splitting the tests into a fixed number of suites.
                LOGGER.warning("Received 503 from Evergreen, "
                               "dividing the tests evenly among suites")
                return self.calculate_fallback_suites()
            else:
                raise

    def get_evg_stats(self, project, start_date, end_date, task, variant):
        """Collect test execution statistics data from Evergreen."""
        # pylint: disable=too-many-arguments

        days = (end_date - start_date).days
        return self.evergreen_api.test_stats_by_project(
            project, after_date=start_date.strftime("%Y-%m-%d"),
            before_date=end_date.strftime("%Y-%m-%d"), tasks=[task], variants=[variant],
            group_by="test", group_num_days=days)

    def calculate_suites_from_evg_stats(self, data, execution_time_secs):
        """Divide tests into suites that can be run in less than the specified execution time."""
        test_stats = teststats.TestStats(data)
        tests_runtimes = self.filter_existing_tests(test_stats.get_tests_runtimes())
        if not tests_runtimes:
            LOGGER.debug("No test runtimes after filter, using fallback")
            return self.calculate_fallback_suites()
        self.test_list = [info.test_name for info in tests_runtimes]
        return divide_tests_into_suites(self.config_options.suite, tests_runtimes,
                                        execution_time_secs, self.config_options.max_sub_suites,
                                        self.config_options.max_tests_per_suite)

    def filter_existing_tests(self, tests_runtimes):
        """Filter out tests that do not exist in the filesystem."""
        all_tests = [teststats.normalize_test_name(test) for test in self.list_tests()]
        return [
            info for info in tests_runtimes
            if os.path.exists(info.test_name) and info.test_name in all_tests
        ]

    def calculate_fallback_suites(self):
        """Divide tests into a fixed number of suites."""
        LOGGER.debug("Splitting tasks based on fallback",
                     fallback=self.config_options.fallback_num_sub_suites)
        num_suites = self.config_options.fallback_num_sub_suites
        self.test_list = self.list_tests()
        suites = [Suite(self.config_options.suite) for _ in range(num_suites)]
        for idx, test_file in enumerate(self.test_list):
            suites[idx % num_suites].add_test(test_file, 0)
        return suites

    def list_tests(self):
        """List the test files that are part of the suite being split."""
        return suitesconfig.get_suite(self.config_options.suite).tests

    def render_evergreen_config(self, suites: List[Suite], task: str) -> Tuple[str, str]:
        """Generate the evergreen configuration for the new suite and write it to disk."""
        evg_config_gen = EvergreenConfigGenerator(suites, self.config_options, self.evergreen_api)
        evg_config = evg_config_gen.generate_config()
        return task + ".json", evg_config.to_json()

    def run(self):
        """Generate resmoke suites that run within a specified target execution time."""
        LOGGER.debug("config options", config_options=self.config_options)

        if not should_tasks_be_generated(self.evergreen_api, self.config_options.task_id):
            LOGGER.info("Not generating configuration due to previous successful generation.")
            return

        end_date = datetime.datetime.utcnow().replace(microsecond=0)
        start_date = end_date - datetime.timedelta(days=LOOKBACK_DURATION_DAYS)
        target_dir = self.config_options.generated_config_dir

        suites = self.calculate_suites(start_date, end_date)

        LOGGER.debug("Creating suites", num_suites=len(suites), task=self.config_options.task,
                     dir=target_dir)
        config_file_dict = render_suite_files(suites, self.config_options.suite, self.test_list,
                                              self.config_options.test_suites_dir)

        shrub_config = self.render_evergreen_config(suites, self.config_options.task)
        config_file_dict[shrub_config[0]] = shrub_config[1]

        write_file_dict(target_dir, config_file_dict)


@click.command()
@click.option("--expansion-file", type=str, required=True,
              help="Location of expansions file generated by evergreen.")
@click.option("--evergreen-config", type=str, default=CONFIG_FILE,
              help="Location of evergreen configuration file.")
@click.option("--verbose", is_flag=True, default=False, help="Enable verbose logging.")
def main(expansion_file, evergreen_config, verbose):
    """
    Create a configuration for generate tasks to create sub suites for the specified resmoke suite.

    The `--expansion-file` should contain all the configuration needed to generate the tasks.
    \f
    :param expansion_file: Configuration file.
    :param evergreen_config: Evergreen configuration file.
    :param verbose: Use verbose logging.
    """
    enable_logging(verbose)
    evg_api = RetryingEvergreenApi.get_api(config_file=evergreen_config)
    config_options = ConfigOptions.from_file(expansion_file, REQUIRED_CONFIG_KEYS,
                                             DEFAULT_CONFIG_VALUES, CONFIG_FORMAT_FN)

    GenerateSubSuites(evg_api, config_options).run()


if __name__ == "__main__":
    main()  # pylint: disable=no-value-for-parameter
