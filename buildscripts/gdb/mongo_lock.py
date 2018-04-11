"""Mongo lock module."""

from __future__ import print_function

import re
import sys

import gdb
import gdb.printing

if sys.version_info[0] >= 3:
    # GDB only permits converting a gdb.Value instance to its numerical address when using the
    # long() constructor in Python 2 and not when using the int() constructor. We define the
    # 'long' class as an alias for the 'int' class in Python 3 for compatibility.
    long = int  # pylint: disable=redefined-builtin,invalid-name


class Thread(object):
    """Thread class."""

    def __init__(self, thread_id, lwpid, thread_name):
        """Initialize Thread."""
        self.thread_id = thread_id
        self.lwpid = lwpid
        self.name = thread_name

    def __eq__(self, other):
        if isinstance(other, Thread):
            return self.thread_id == other.thread_id
        return NotImplemented

    def __ne__(self, other):
        return not self == other

    def __str__(self):
        return "{} (Thread 0x{:012x} (LWP {}))".format(self.name, self.thread_id, self.lwpid)

    def key(self):
        """Return thread key."""
        return "Thread 0x{:012x}".format(self.thread_id)


class Lock(object):
    """Lock class."""

    def __init__(self, addr, resource):
        """Initialize Lock."""
        self.addr = addr
        self.resource = resource

    def __eq__(self, other):
        if isinstance(other, Lock):
            return self.addr == other.addr
        return NotImplemented

    def __ne__(self, other):
        return not self == other

    def __str__(self):
        return "Lock 0x{:012x} ({})".format(self.addr, self.resource)

    def key(self):
        """Return lock key."""
        return "Lock 0x{:012x}".format(self.addr)


class Graph(object):
    """Graph class.

    The Graph is a dict with the following structure:
      {'node_key': {'node': {id: val}, 'next_nodes': [node_key_1, ...]}}
    Example graph:
      {
       'Lock 1': {'node': {1: 'MongoDB lock'}, 'next_nodes': ['Thread 1']},
       'Lock 2': {'node': {2: 'MongoDB lock'}, 'next_nodes': ['Thread 2']},
       'Thread 1': {'node': {1: 123}, 'next_nodes': ['Lock 2']},
       'Thread 2': {'node': {2: 456}, 'next_nodes': ['Lock 1']}
      }
    """

    def __init__(self):
        """Initialize Graph."""
        self.nodes = {}

    def is_empty(self):
        """Return True if graph is empty."""
        return not bool(self.nodes)

    def add_node(self, node):
        """Add node to graph."""
        if not self.find_node(node):
            self.nodes[node.key()] = {'node': node, 'next_nodes': []}

    def find_node(self, node):
        """Find node in graph."""
        if node.key() in self.nodes:
            return self.nodes[node.key()]
        return None

    def find_from_node(self, from_node):
        """Find from node."""
        for node_key in self.nodes:
            node = self.nodes[node_key]
            for next_node in node['next_nodes']:
                if next_node == from_node['node'].key():
                    return node
        return None

    def remove_nodes_without_edge(self):
        """Remove nodes without edge."""
        # Rebuild graph by removing any nodes which do not have any incoming or outgoing edges.
        temp_nodes = {}
        for node_key in self.nodes:
            node = self.nodes[node_key]
            if node['next_nodes'] or self.find_from_node(node) is not None:
                temp_nodes[node_key] = self.nodes[node_key]
        self.nodes = temp_nodes

    def add_edge(self, from_node, to_node):
        """Add edge."""
        f_node = self.find_node(from_node)
        if f_node is None:
            self.add_node(from_node)
            f_node = self.nodes[from_node.key()]

        t_node = self.find_node(to_node)
        if t_node is None:
            self.add_node(to_node)
            t_node = self.nodes[to_node.key()]

        for n_node in f_node['next_nodes']:
            if n_node == to_node.key():
                return
        self.nodes[from_node.key()]['next_nodes'].append(to_node.key())

    def print(self):
        """Print graph."""
        for node_key in self.nodes:
            print("Node", self.nodes[node_key]['node'])
            for to_node in self.nodes[node_key]['next_nodes']:
                print(" ->", to_node)

    def to_graph(self, nodes=None, message=None):
        """Return the 'to_graph'."""
        sb = []
        sb.append('# Legend:')
        sb.append('#    Thread 1 -> Lock 1 indicates Thread 1 is waiting on Lock 1')
        sb.append('#    Lock 2 -> Thread 2 indicates Lock 2 is held by Thread 2')
        if message is not None:
            sb.append(message)
        sb.append('digraph "mongod+lock-status" {')
        for node_key in self.nodes:
            for next_node_key in self.nodes[node_key]['next_nodes']:
                sb.append('    "{}" -> "{}";'.format(node_key, next_node_key))
        for node_key in self.nodes:
            color = ""
            if nodes and node_key in nodes:
                color = "color = red"
            sb.append('    "{}" [label="{}" {}]'.format(node_key, self.nodes[node_key]['node'],
                                                        color))
        sb.append("}")
        return "\n".join(sb)

    def depth_first_search(self, node_key, nodes_visited, nodes_in_cycle=None):
        """Perform depth first search and return the list of nodes in the cycle or None.

        The nodes_visited is a set of nodes which indicates it has been visited.
        The node_in_cycle is a list of nodes in the potential cycle.
        """
        if nodes_in_cycle is None:
            nodes_in_cycle = []
        nodes_visited.add(node_key)
        nodes_in_cycle.append(node_key)
        for node in self.nodes[node_key]['next_nodes']:
            if node in nodes_in_cycle:
                # The graph cycle starts at the index of node in nodes_in_cycle.
                return nodes_in_cycle[nodes_in_cycle.index(node):]
            if node not in nodes_visited:
                dfs_nodes = self.depth_first_search(node, nodes_visited, nodes_in_cycle)
                if dfs_nodes:
                    return dfs_nodes

        # This node_key is not part of the graph cycle.
        nodes_in_cycle.pop()
        return None

    def detect_cycle(self):
        """If a cycle is detected, returns a list of nodes in the cycle or None."""
        nodes_visited = set()
        for node in self.nodes:
            if node not in nodes_visited:
                cycle_path = self.depth_first_search(node, nodes_visited)
                if cycle_path:
                    return [str(self.nodes[node_key]['node']) for node_key in cycle_path]
        return None


def find_thread(thread_dict, search_thread_id):
    """Find thread."""
    for (_, thread) in thread_dict.items():
        if thread.thread_id == search_thread_id:
            return thread
    return None


def find_func_block(block):
    """Find func block."""
    while block:
        if block.function:
            return block
        block = block.superblock
    return None


def find_frame(function_name_pattern):
    """Find frame."""
    frame = gdb.newest_frame()
    while frame:
        block = None
        try:
            block = frame.block()
        except RuntimeError as err:
            if err.args[0] != "Cannot locate block for frame.":
                raise

        block = find_func_block(block)
        if block and re.match(function_name_pattern, block.function.name):
            return frame
        try:
            frame = frame.older()
        except gdb.error as err:
            print("Ignoring GDB error '%s' in find_frame" % str(err))
            break
    return None


def find_mutex_holder(graph, thread_dict, show):
    """Find mutex holder."""
    frame = find_frame(r'std::mutex::lock\(\)')
    if frame is None:
        return

    frame.select()

    # Waiting for mutex locking!
    mutex_this, _ = gdb.lookup_symbol("this", frame.block())
    mutex_value = mutex_this.value(frame)
    # The mutex holder is a LWPID
    mutex_holder_lwpid = int(mutex_value["_M_mutex"]["__data"]["__owner"])

    # At time thread_dict was initialized, the mutex holder may not have been found.
    # Use the thread LWP as a substitute for showing output or generating the graph.
    if mutex_holder_lwpid not in thread_dict:
        print("Warning: Mutex at {} held by thread with LWP {}"
              " not found in thread_dict. Using LWP to track thread.".format(
                  mutex_value, mutex_holder_lwpid))
        mutex_holder = Thread(mutex_holder_lwpid, mutex_holder_lwpid, '"[unknown]"')
    else:
        mutex_holder = thread_dict[mutex_holder_lwpid]

    (_, mutex_waiter_lwpid, _) = gdb.selected_thread().ptid
    mutex_waiter = thread_dict[mutex_waiter_lwpid]
    if show:
        print("Mutex at {} held by {} waited on by {}".format(mutex_value, mutex_holder,
                                                              mutex_waiter))
    if graph:
        graph.add_edge(mutex_waiter, Lock(long(mutex_value), "Mutex"))
        graph.add_edge(Lock(long(mutex_value), "Mutex"), mutex_holder)


def find_lock_manager_holders(graph, thread_dict, show):  # pylint: disable=too-many-locals
    """Find lock manager holders."""
    frame = find_frame(r'mongo::LockerImpl\<.*\>::')
    if not frame:
        return

    frame.select()

    (_, lock_waiter_lwpid, _) = gdb.selected_thread().ptid
    lock_waiter = thread_dict[lock_waiter_lwpid]

    locker_ptr_type = gdb.lookup_type("mongo::LockerImpl<false>").pointer()
    lock_head = gdb.parse_and_eval(
        "mongo::getGlobalLockManager()->_getBucket(resId)->findOrInsert(resId)")

    granted_list = lock_head.dereference()["grantedList"]
    lock_request_ptr = granted_list["_front"]
    while lock_request_ptr:
        lock_request = lock_request_ptr.dereference()
        locker_ptr = lock_request["locker"]
        locker_ptr = locker_ptr.cast(locker_ptr_type)
        locker = locker_ptr.dereference()
        lock_holder_id = int(locker["_threadId"]["_M_thread"])
        lock_holder = find_thread(thread_dict, lock_holder_id)
        if show:
            print("MongoDB Lock at {} ({}) held by {} waited on by {}".format(
                lock_head, lock_request["mode"], lock_holder, lock_waiter))
        if graph:
            graph.add_edge(lock_waiter, Lock(long(lock_head), "MongoDB lock"))
            graph.add_edge(Lock(long(lock_head), "MongoDB lock"), lock_holder)
        lock_request_ptr = lock_request["next"]


def get_locks(graph, thread_dict, show=False):
    """Get locks."""
    for thread in gdb.selected_inferior().threads():
        try:
            if not thread.is_valid():
                continue
            thread.switch()
            find_mutex_holder(graph, thread_dict, show)
            find_lock_manager_holders(graph, thread_dict, show)
        except gdb.error as err:
            print("Ignoring GDB error '%s' in get_locks" % str(err))


def get_threads_info():
    """Get threads info."""
    thread_dict = {}
    for thread in gdb.selected_inferior().threads():
        try:
            if not thread.is_valid():
                continue
            thread.switch()
            # PTID is a tuple: Process ID (PID), Lightweight Process ID (LWPID), Thread ID (TID)
            (_, lwpid, _) = thread.ptid
            thread_num = thread.num
            thread_name = get_current_thread_name()  # pylint: disable=undefined-variable
            thread_id = get_thread_id()  # pylint: disable=undefined-variable
            if not thread_id:
                print("Unable to retrieve thread_info for thread %d" % thread_num)
                continue
            thread_dict[lwpid] = Thread(thread_id, lwpid, thread_name)
        except gdb.error as err:
            print("Ignoring GDB error '%s' in get_threads_info" % str(err))

    return thread_dict


class MongoDBShowLocks(gdb.Command):
    """Show MongoDB locks & pthread mutexes."""

    def __init__(self):
        """Initialize MongoDBShowLocks."""
        RegisterMongoCommand.register(  # pylint: disable=undefined-variable
            self, "mongodb-show-locks", gdb.COMMAND_DATA)

    def invoke(self, *_):
        """Invoke mongodb_show_locks."""
        self.mongodb_show_locks()

    @staticmethod
    def mongodb_show_locks():
        """GDB in-process python supplement."""
        try:
            thread_dict = get_threads_info()
            get_locks(graph=None, thread_dict=thread_dict, show=True)
        except gdb.error as err:
            print("Ignoring GDB error '%s' in mongodb_show_locks" % str(err))


MongoDBShowLocks()


class MongoDBWaitsForGraph(gdb.Command):
    """Create MongoDB WaitsFor lock graph [graph_file]."""

    def __init__(self):
        """Initialize MongoDBWaitsForGraph."""
        RegisterMongoCommand.register(  # pylint: disable=undefined-variable
            self, "mongodb-waitsfor-graph", gdb.COMMAND_DATA)

    def invoke(self, arg, *_):
        """Invoke mongodb_waitsfor_graph."""
        self.mongodb_waitsfor_graph(arg)

    @staticmethod
    def mongodb_waitsfor_graph(graph_file=None):
        """GDB in-process python supplement."""

        graph = Graph()
        try:
            thread_dict = get_threads_info()
            get_locks(graph=graph, thread_dict=thread_dict, show=False)
            graph.remove_nodes_without_edge()
            if graph.is_empty():
                print("Not generating the digraph, since the lock graph is empty")
                return
            cycle_message = "# No cycle detected in the graph"
            cycle_nodes = graph.detect_cycle()
            if cycle_nodes:
                cycle_message = "# Cycle detected in the graph nodes %s" % cycle_nodes
            if graph_file:
                print("Saving digraph to %s" % graph_file)
                with open(graph_file, 'w') as fh:
                    fh.write(graph.to_graph(nodes=cycle_nodes, message=cycle_message))
                print(cycle_message.split("# ")[1])
            else:
                print(graph.to_graph(nodes=cycle_nodes, message=cycle_message))

        except gdb.error as err:
            print("Ignoring GDB error '%s' in mongod_deadlock_graph" % str(err))


MongoDBWaitsForGraph()

print("MongoDB Lock analysis commands loaded")
