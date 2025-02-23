#
# Copyright (C) 2022-present ScyllaDB
#
# SPDX-License-Identifier: AGPL-3.0-or-later
#

import asyncio
import pytest
import logging
import time
import functools
from typing import Callable, Awaitable, Optional, TypeVar, Generic

from cassandra.cluster import NoHostAvailable, Session
from cassandra.pool import Host

from test.pylib.manager_client import ManagerClient
from test.pylib.random_tables import RandomTables
from test.pylib.rest_client import ScyllaRESTAPIClient, inject_error


T = TypeVar('T')


async def reconnect_driver(manager: ManagerClient) -> Session:
    """Workaround for scylladb/python-driver#170:
       the existing driver session may not reconnect, create a new one.
    """
    logging.info(f"Reconnecting driver")
    manager.driver_close()
    await manager.driver_connect()
    cql = manager.cql
    assert(cql)
    return cql


async def restart(manager: ManagerClient, srv: str) -> None:
    logging.info(f"Stopping {srv} gracefully")
    await manager.server_stop_gracefully(srv)
    logging.info(f"Restarting {srv}")
    await manager.server_start(srv)
    logging.info(f"{srv} restarted")


async def enable_raft(manager: ManagerClient, srv: str) -> None:
    config = await manager.server_get_config(srv)
    features = config['experimental_features']
    assert(type(features) == list)
    features.append('raft')
    logging.info(f"Updating config of server {srv}")
    await manager.server_update_config(srv, 'experimental_features', features)


async def enable_raft_and_restart(manager: ManagerClient, srv: str) -> None:
    await enable_raft(manager, srv)
    await restart(manager, srv)


async def wait_for(pred: Callable[[], Awaitable[Optional[T]]], deadline: float) -> T:
    while True:
        assert(time.time() < deadline), "Deadline exceeded, failing test."
        res = await pred()
        if res is not None:
            return res
        await asyncio.sleep(1)


async def wait_for_cql(cql: Session, host: Host, deadline: float) -> None:
    async def cql_ready():
        try:
            await cql.run_async("select * from system.local", host=host)
        except NoHostAvailable:
            logging.info(f"Driver not connected to {host} yet")
            return None
        return True
    await wait_for(cql_ready, deadline)


async def wait_for_cql_and_get_hosts(cql: Session, ips: list[str], deadline: float) -> list[Host]:
    """Wait until every ip in `ips` is available through `cql` and translate `ips` to `Host`s."""
    ip_set = set(ips)
    async def get_hosts() -> Optional[list[Host]]:
        hosts = cql.cluster.metadata.all_hosts()
        remaining = ip_set - {h.address for h in hosts}
        if not remaining:
            return hosts

        logging.info(f"Driver hasn't yet learned about hosts: {remaining}")
        return None
    hosts = await wait_for(get_hosts, deadline)

    # Take only hosts from `ip_set` (there may be more)
    hosts = [h for h in hosts if h.address in ip_set]
    await asyncio.gather(*(wait_for_cql(cql, h, deadline) for h in hosts))

    return hosts


async def wait_for_upgrade_state(state: str, cql: Session, host: Host, deadline: float) -> None:
    """Wait until group 0 upgrade state reaches `state` on `host`, using `cql` to query it.  Warning: if the
       upgrade procedure may progress beyond `state` this function may not notice when it entered `state` and
       then time out.  Use it only if either `state` is the last state or the conditions of the test don't allow
       the upgrade procedure to progress beyond `state` (e.g. a dead node causing the procedure to be stuck).
    """
    async def reached_state():
        rs = await cql.run_async("select value from system.scylla_local where key = 'group0_upgrade_state'", host=host)
        if rs:
            value = rs[0].value
            if value == state:
                return True
            else:
                logging.info(f"Upgrade not yet in state {state} on server {host}, state: {value}")
        else:
            logging.info(f"Upgrade not yet in state {state} on server {host}, no state was written")
        return None
    await wait_for(reached_state, deadline)


async def wait_until_upgrade_finishes(cql: Session, host: Host, deadline: float) -> None:
    await wait_for_upgrade_state('use_post_raft_procedures', cql, host, deadline)


async def wait_for_gossip_gen_increase(api: ScyllaRESTAPIClient, gen: int, node_ip: str, target_ip: str, deadline: float):
    """Wait until the generation number of `target_ip` increases above `gen` from the point of view of `node_ip`.
       Can be used to wait until `node_ip` gossips with `target_ip` after `target_ip` was restarted
       by saving the generation number of `target_ip` before restarting it and then calling this function
       (nodes increase their generation numbers when they restart).
    """
    async def gen_increased() -> Optional[int]:
        curr_gen = await api.get_gossip_generation_number(node_ip, target_ip)
        if curr_gen <= gen:
            logging.info(f"Gossip generation number of {target_ip} is {curr_gen} <= {gen} according to {node_ip}")
            return None
        return curr_gen
    gen = await wait_for(gen_increased, deadline)
    logging.info(f"Gossip generation number of {target_ip} is reached {gen} according to {node_ip}")


async def delete_raft_data(cql: Session, host: Host) -> None:
    await cql.run_async("truncate table system.discovery", host=host)
    await cql.run_async("truncate table system.group0_history", host=host)
    await cql.run_async("delete value from system.scylla_local where key = 'raft_group0_id'", host=host)


def log_run_time(f):
    @functools.wraps(f)
    async def wrapped(*args, **kwargs):
        start = time.time()
        res = await f(*args, **kwargs)
        logging.info(f"{f.__name__} took {int(time.time() - start)} seconds.")
        return res
    return wrapped


@pytest.mark.asyncio
@log_run_time
async def test_raft_upgrade_basic(manager: ManagerClient, random_tables: RandomTables):
    """
    kbr-: the test takes about 7 seconds in dev mode on my laptop.
    """
    servers = await manager.running_servers()
    cql = manager.cql
    assert(cql)

    # system.group0_history should either not exist or there should be no entries in it before upgrade.
    if await cql.run_async("select * from system_schema.tables where keyspace_name = 'system' and table_name = 'group0_history'"):
        assert(not (await cql.run_async("select * from system.group0_history")))

    logging.info(f"Enabling Raft on {servers} and restarting")
    await asyncio.gather(*(enable_raft_and_restart(manager, srv) for srv in servers))
    cql = await reconnect_driver(manager)

    logging.info("Cluster restarted, waiting until driver reconnects to every server")
    hosts = await wait_for_cql_and_get_hosts(cql, servers, time.time() + 60)

    logging.info(f"Driver reconnected, hosts: {hosts}. Waiting until upgrade finishes")
    await asyncio.gather(*(wait_until_upgrade_finishes(cql, h, time.time() + 60) for h in hosts))

    logging.info("Upgrade finished. Creating a new table")
    table = await random_tables.add_table(ncolumns=5)

    logging.info("Checking group0_history")
    rs = await cql.run_async("select * from system.group0_history")
    assert(rs)
    logging.info(f"group0_history entry description: '{rs[0].description}'")
    assert(table.full_name in rs[0].description)


@pytest.mark.asyncio
@log_run_time
async def test_raft_upgrade_with_node_remove(manager: ManagerClient, random_tables: RandomTables):
    """
    We enable Raft on every server but stop one server in the meantime.
    The others will start Raft upgrade procedure but get stuck - all servers must be available to proceed.
    We remove the stopped server and check that the procedure unblocks.

    kbr-: the test takes about 19 seconds in dev mode on my laptop.
    """
    servers = await manager.running_servers()
    srv1, *others = servers

    srv1_gen = await manager.api.get_gossip_generation_number(others[0], srv1)
    logging.info(f"Gossip generation number of {srv1} seen from {others[0]}: {srv1_gen}")

    logging.info(f"Enabling Raft on {srv1} and restarting")
    await enable_raft_and_restart(manager, srv1)

    # Before continuing, ensure that another node has gossiped with srv1
    # after srv1 has restarted. Then we know that the other node learned about srv1's
    # supported features, including SUPPORTS_RAFT.
    logging.info(f"Waiting until {others[0]} gossips with {srv1}")
    await wait_for_gossip_gen_increase(manager.api, srv1_gen, others[0], srv1, time.time() + 60)

    srv1_host_id = await manager.get_host_id(srv1)
    logging.info(f"Obtained host ID of {srv1}: {srv1_host_id}")

    logging.info(f"Stopping {srv1}")
    await manager.server_stop_gracefully(srv1)

    logging.info(f"Enabling Raft on {others} and restarting")
    await asyncio.gather(*(enable_raft_and_restart(manager, srv) for srv in others))
    cql = await reconnect_driver(manager)

    logging.info(f"Cluster restarted, waiting until driver reconnects to every server except {srv1}")
    hosts = await wait_for_cql_and_get_hosts(cql, others, time.time() + 60)
    logging.info(f"Driver reconnected, hosts: {hosts}")

    logging.info(f"Removing {srv1} using {others[0]}")
    await manager.remove_node(others[0], srv1, srv1_host_id)

    logging.info("Waiting until upgrade finishes")
    await asyncio.gather(*(wait_until_upgrade_finishes(cql, h, time.time() + 60) for h in hosts))


@pytest.mark.asyncio
@log_run_time
async def test_recover_stuck_raft_upgrade(manager: ManagerClient, random_tables: RandomTables):
    """
    We enable Raft on every server and the upgrade procedure starts.  All servers join group 0. Then one
    of them fails, the rest enter 'synchronize' state.  We assume the failed server cannot be recovered.
    We cannot just remove it at this point; it's already part of group 0, `remove_from_group0` will wait
    until upgrade procedure finishes - but the procedure is stuck.  To proceed we enter RECOVERY state on
    the other servers, remove the failed one, and clear existing Raft data. After leaving RECOVERY the
    remaining nodes will restart the procedure, establish a new group 0 and finish upgrade.

    kbr-: the test takes about 26 seconds in dev mode on my laptop.
    """
    servers = await manager.running_servers()
    srv1, *others = servers

    logging.info(f"Enabling Raft on {srv1} and restarting")
    await enable_raft_and_restart(manager, srv1)

    # TODO error injection should probably be done through ScyllaClusterManager (we may need to mark the cluster as dirty).
    # In this test the cluster is dirty anyway due to a restart so it's safe.
    async with inject_error(manager.api, srv1, 'group0_upgrade_before_synchronize', one_shot=True):
        logging.info(f"Enabling Raft on {others} and restarting")
        await asyncio.gather(*(enable_raft_and_restart(manager, srv) for srv in others))
        cql = await reconnect_driver(manager)

        logging.info(f"Cluster restarted, waiting until driver reconnects to {others}")
        hosts = await wait_for_cql_and_get_hosts(cql, others, time.time() + 60)
        logging.info(f"Driver reconnected, hosts: {hosts}")

        logging.info(f"Waiting until {hosts} enter 'synchronize' state")
        await asyncio.gather(*(wait_for_upgrade_state('synchronize', cql, h, time.time() + 60) for h in hosts))
        logging.info(f"{hosts} entered synchronize")

        # TODO ensure that srv1 failed upgrade - look at logs?
        # '[shard 0] raft_group0_upgrade - Raft upgrade failed: std::runtime_error (error injection before group 0 upgrade enters synchronize).'

    logging.info(f"Setting recovery state on {hosts}")
    for host in hosts:
        await cql.run_async("update system.scylla_local set value = 'recovery' where key = 'group0_upgrade_state'", host=host)

    logging.info(f"Restarting {others}")
    await asyncio.gather(*(restart(manager, srv) for srv in others))
    cql = await reconnect_driver(manager)

    logging.info(f"{others} restarted, waiting until driver reconnects to them")
    hosts = await wait_for_cql_and_get_hosts(cql, others, time.time() + 60)

    logging.info(f"Checking if {hosts} are in recovery state")
    for host in hosts:
        rs = await cql.run_async("select value from system.scylla_local where key = 'group0_upgrade_state'", host=host)
        assert rs[0].value == 'recovery'

    logging.info("Creating a table while in recovery state")
    table = await random_tables.add_table(ncolumns=5)

    srv1_host_id = await manager.get_host_id(srv1)
    logging.info(f"Obtained host ID of {srv1}: {srv1_host_id}")

    logging.info(f"Stopping {srv1}")
    await manager.server_stop_gracefully(srv1)

    logging.info(f"Removing {srv1} using {others[0]}")
    await manager.remove_node(others[0], srv1, srv1_host_id)

    logging.info(f"Deleting Raft data and upgrade state on {hosts} and restarting")
    for host in hosts:
        await delete_raft_data(cql, host)
        await cql.run_async("delete from system.scylla_local where key = 'group0_upgrade_state'", host=host)

    await asyncio.gather(*(restart(manager, srv) for srv in others))
    cql = await reconnect_driver(manager)

    logging.info(f"Cluster restarted, waiting until driver reconnects to {others}")
    hosts = await wait_for_cql_and_get_hosts(cql, others, time.time() + 60)

    logging.info(f"Driver reconnected, hosts: {hosts}, waiting until upgrade finishes")
    await asyncio.gather(*(wait_until_upgrade_finishes(cql, h, time.time() + 60) for h in hosts))

    logging.info("Checking if previously created table still exists")
    await cql.run_async(f"select * from {table.full_name}")


@pytest.mark.asyncio
@log_run_time
async def test_recovery_after_majority_loss(manager: ManagerClient, random_tables: RandomTables):
    """
    We successfully upgrade a cluster. Eventually however all servers but one fail - group 0
    is left without a majority. We create a new group 0 by entering RECOVERY, using `removenode`
    to get rid of the other servers, clearing Raft data and restarting. The Raft upgrade procedure
    runs again to establish a single-node group 0. We also verify that schema changes performed
    using the old group 0 are still there.
    Note: in general there's no guarantee that all schema changes will be present; the minority
    used to recover group 0 might have missed them. However in this test the driver waits
    for schema agreement to complete before proceeding, so we know that every server learned
    about the schema changes.

    kbr-: the test takes about 22 seconds in dev mode on my laptop.
    """
    servers = await manager.running_servers()

    logging.info(f"Enabling Raft on {servers} and restarting")
    await asyncio.gather(*(enable_raft_and_restart(manager, srv) for srv in servers))
    cql = await reconnect_driver(manager)

    logging.info("Cluster restarted, waiting until driver reconnects to every server")
    hosts = await wait_for_cql_and_get_hosts(cql, servers, time.time() + 60)

    logging.info(f"Driver reconnected, hosts: {hosts}. Waiting until upgrade finishes")
    await asyncio.gather(*(wait_until_upgrade_finishes(cql, h, time.time() + 60) for h in hosts))

    logging.info("Upgrade finished. Creating a bunch of tables")
    tables = await asyncio.gather(*(random_tables.add_table(ncolumns=5) for _ in range(5)))

    srv1, *others = servers
    others_with_host_ids = [(srv, await manager.get_host_id(srv)) for srv in others]
    logging.info(f"Obtained host IDs: {others_with_host_ids}")

    logging.info(f"Killing all nodes except {srv1}")
    await asyncio.gather(*(manager.server_stop(srv) for srv in others))

    logging.info(f"Entering recovery state on {srv1}")
    host1 = next(h for h in hosts if h.address == srv1)
    await cql.run_async("update system.scylla_local set value = 'recovery' where key = 'group0_upgrade_state'", host=host1)
    await restart(manager, srv1)
    cql = await reconnect_driver(manager)

    logging.info("Node restarted, waiting until driver connects")
    host1 = (await wait_for_cql_and_get_hosts(cql, [srv1], time.time() + 60))[0]

    for i in range(len(others_with_host_ids)):
        remove_ip, remove_host_id = others_with_host_ids[i]
        ignore_dead_ips = [ip for (ip, _) in others_with_host_ids[i+1:]]
        logging.info(f"Removing {remove_ip} using {srv1} with ignore_dead: {ignore_dead_ips}")
        await manager.remove_node(srv1, remove_ip, remove_host_id, ignore_dead_ips)

    logging.info(f"Deleting old Raft data and upgrade state on {host1} and restarting")
    await delete_raft_data(cql, host1)
    await cql.run_async("delete from system.scylla_local where key = 'group0_upgrade_state'", host=host1)
    await restart(manager, srv1)
    cql = await reconnect_driver(manager)

    logging.info("Node restarted, waiting until driver connects")
    host1 = (await wait_for_cql_and_get_hosts(cql, [srv1], time.time() + 60))[0]

    logging.info(f"Driver reconnected, host: {host1}. Waiting until upgrade finishes.")
    await wait_until_upgrade_finishes(cql, host1, time.time() + 60)

    logging.info("Checking if previously created tables still exist")
    await asyncio.gather(*(cql.run_async(f"select * from {t.full_name}") for t in tables))

    logging.info("Creating another table")
    await random_tables.add_table(ncolumns=5)
