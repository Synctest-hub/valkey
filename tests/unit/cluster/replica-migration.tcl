# Allocate slot 0 to the last primary and evenly distribute the remaining
# slots to the remaining primaries.
proc my_slot_allocation {masters replicas} {
    set avg [expr double(16384) / [expr $masters-1]]
    set slot_start 1
    for {set j 0} {$j < $masters-1} {incr j} {
        set slot_end [expr int(ceil(($j + 1) * $avg) - 1)]
        R $j cluster addslotsrange $slot_start $slot_end
        set slot_start [expr $slot_end + 1]
    }
    R [expr $masters-1] cluster addslots 0
}

proc test_migrated_replica {type} {
    test "Migrated replica reports zero repl offset and rank, and fails to win election - $type" {
        # Write some data to primary 0, slot 1, make a small repl_offset.
        for {set i 0} {$i < 1024} {incr i} {
            R 0 incr key_991803
        }
        assert_equal {1024} [R 0 get key_991803]

        # Write some data to primary 3, slot 0, make a big repl_offset.
        for {set i 0} {$i < 10240} {incr i} {
            R 3 incr key_977613
        }
        assert_equal {10240} [R 3 get key_977613]

        # 10s, make sure primary 0 will hang in the save.
        R 0 config set rdb-key-save-delay 100000000

        # Move the slot 0 from primary 3 to primary 0
        set addr "[srv 0 host]:[srv 0 port]"
        set myid [R 3 CLUSTER MYID]
        set code [catch {
            exec src/valkey-cli {*}[valkeycli_tls_config "./tests"] --cluster rebalance $addr --cluster-weight $myid=0
        } result]
        if {$code != 0} {
            fail "valkey-cli --cluster rebalance returns non-zero exit code, output below:\n$result"
        }

        # Validate that shard 3's primary and replica can convert to replicas after
        # they lose the last slot.
        R 3 config set cluster-replica-validity-factor 0
        R 7 config set cluster-replica-validity-factor 0
        R 3 config set cluster-allow-replica-migration yes
        R 7 config set cluster-allow-replica-migration yes

        if {$type == "shutdown"} {
            # Shutdown primary 0.
            catch {R 0 shutdown nosave}
        } elseif {$type == "sigstop"} {
            # Pause primary 0.
            set primary0_pid [s 0 process_id]
            pause_process $primary0_pid
        }

        # Wait for the replica to become a primary, and make sure
        # the other primary become a replica.
        wait_for_condition 1000 50 {
            [s -4 role] eq {master} &&
            [s -3 role] eq {slave} &&
            [s -7 role] eq {slave}
        } else {
            puts "s -4 role: [s -4 role]"
            puts "s -3 role: [s -3 role]"
            puts "s -7 role: [s -7 role]"
            fail "Failover does not happened"
        }

        # Make sure the offset of server 3 / 7 is 0.
        verify_log_message -3 "*Start of election*offset 0*" 0
        verify_log_message -7 "*Start of election*offset 0*" 0

        # Make sure the right replica gets the higher rank.
        verify_log_message -4 "*Start of election*rank #0*" 0

        # Wait for the cluster to be ok.
        wait_for_condition 1000 50 {
            [CI 3 cluster_state] eq "ok" &&
            [CI 4 cluster_state] eq "ok" &&
            [CI 7 cluster_state] eq "ok"
        } else {
            puts "R 3: [R 3 cluster info]"
            puts "R 4: [R 4 cluster info]"
            puts "R 7: [R 7 cluster info]"
            fail "Cluster is down"
        }

        # Make sure the key exists and is consistent.
        R 3 readonly
        R 7 readonly
        wait_for_condition 1000 50 {
            [R 3 get key_991803] == 1024 && [R 3 get key_977613] == 10240 &&
            [R 4 get key_991803] == 1024 && [R 4 get key_977613] == 10240 &&
            [R 7 get key_991803] == 1024 && [R 7 get key_977613] == 10240
        } else {
            puts "R 3: [R 3 keys *]"
            puts "R 4: [R 4 keys *]"
            puts "R 7: [R 7 keys *]"
            fail "Key not consistent"
        }

        if {$type == "sigstop"} {
            resume_process $primary0_pid

            # Wait for the old primary to go online and become a replica.
            wait_for_condition 1000 50 {
                [s 0 role] eq {slave}
            } else {
                fail "The old primary was not converted into replica"
            }
        }
    }
} ;# proc

start_cluster 4 4 {tags {external:skip cluster} overrides {cluster-node-timeout 1000 cluster-migration-barrier 999}} {
    test_migrated_replica "shutdown"
} my_slot_allocation cluster_allocate_replicas ;# start_cluster

start_cluster 4 4 {tags {external:skip cluster} overrides {cluster-node-timeout 1000 cluster-migration-barrier 999}} {
    test_migrated_replica "sigstop"
} my_slot_allocation cluster_allocate_replicas ;# start_cluster

proc test_nonempty_replica {type} {
    test "New non-empty replica reports zero repl offset and rank, and fails to win election - $type" {
        # Write some data to primary 0, slot 1, make a small repl_offset.
        for {set i 0} {$i < 1024} {incr i} {
            R 0 incr key_991803
        }
        assert_equal {1024} [R 0 get key_991803]

        # Write some data to primary 3, slot 0, make a big repl_offset.
        for {set i 0} {$i < 10240} {incr i} {
            R 3 incr key_977613
        }
        assert_equal {10240} [R 3 get key_977613]

        # 10s, make sure primary 0 will hang in the save.
        R 0 config set rdb-key-save-delay 100000000

        # Make server 7 a replica of server 0.
        R 7 config set cluster-replica-validity-factor 0
        R 7 config set cluster-allow-replica-migration yes
        R 7 cluster replicate [R 0 cluster myid]

        if {$type == "shutdown"} {
            # Shutdown primary 0.
            catch {R 0 shutdown nosave}
        } elseif {$type == "sigstop"} {
            # Pause primary 0.
            set primary0_pid [s 0 process_id]
            pause_process $primary0_pid
        }

        # Wait for the replica to become a primary.
        wait_for_condition 1000 50 {
            [s -4 role] eq {master} &&
            [s -7 role] eq {slave}
        } else {
            puts "s -4 role: [s -4 role]"
            puts "s -7 role: [s -7 role]"
            fail "Failover does not happened"
        }

        # Make sure server 7 gets the lower rank and it's offset is 0.
        verify_log_message -4 "*Start of election*rank #0*" 0
        verify_log_message -7 "*Start of election*offset 0*" 0

        # Wait for the cluster to be ok.
        wait_for_condition 1000 50 {
            [CI 4 cluster_state] eq "ok" &&
            [CI 7 cluster_state] eq "ok"
        } else {
            puts "R 4: [R 4 cluster info]"
            puts "R 7: [R 7 cluster info]"
            fail "Cluster is down"
        }

        # Make sure the key exists and is consistent.
        R 7 readonly
        wait_for_condition 1000 50 {
            [R 4 get key_991803] == 1024 &&
            [R 7 get key_991803] == 1024
        } else {
            puts "R 4: [R 4 get key_991803]"
            puts "R 7: [R 7 get key_991803]"
            fail "Key not consistent"
        }

        if {$type == "sigstop"} {
            resume_process $primary0_pid

            # Waiting the old primary go online and become a replica.
            wait_for_condition 1000 50 {
                [s 0 role] eq {slave}
            } else {
                fail "The old primary was not converted into replica"
            }
        }
    }
} ;# proc

start_cluster 4 4 {tags {external:skip cluster} overrides {cluster-node-timeout 1000 cluster-migration-barrier 999}} {
    test_nonempty_replica "shutdown"
} my_slot_allocation cluster_allocate_replicas ;# start_cluster

start_cluster 4 4 {tags {external:skip cluster} overrides {cluster-node-timeout 1000 cluster-migration-barrier 999}} {
    test_nonempty_replica "sigstop"
} my_slot_allocation cluster_allocate_replicas ;# start_cluster

start_cluster 4 4 {tags {external:skip cluster} overrides {cluster-node-timeout 1000 cluster-migration-barrier 999}} {
    test "valkey-cli make source node ignores NOREPLICAS error when doing the last CLUSTER SETSLOT" {
        R 3 config set cluster-allow-replica-migration no
        R 7 config set cluster-allow-replica-migration yes

        # Record the current primary node, server 7 will be migrated later.
        set old_role_response [R 7 role]
        set old_primary_ip [lindex [R 7 role] 1]
        set old_primary_port [lindex [R 7 role] 2]

        # Move slot 0 from primary 3 to primary 0.
        set addr "[srv 0 host]:[srv 0 port]"
        set myid [R 3 CLUSTER MYID]
        set code [catch {
            exec src/valkey-cli {*}[valkeycli_tls_config "./tests"] --cluster rebalance $addr --cluster-weight $myid=0
        } result]
        if {$code != 0} {
            fail "valkey-cli --cluster rebalance returns non-zero exit code, output below:\n$result"
        }

        # Wait for server 7 role response to change.
        wait_for_condition 1000 50 {
            [R 7 role] ne $old_role_response
        } else {
            puts "R 3 role: [R 3 role]"
            puts "R 7 role: [R 7 role]"
            fail "Server 7 role response has not changed"
        }

        wait_for_cluster_propagation
        wait_for_cluster_state "ok"

        # Make sure server 3 is still a primary and has no replicas.
        assert_equal [s -3 role] {master}
        assert_equal [lindex [R 3 role] 2] {}

        # And server 7 becomes a replica of another primary.
        set new_primary_ip [lindex [R 7 role] 1]
        set new_primary_port [lindex [R 7 role] 2]
        assert_equal [s -7 role] {slave}
        assert_not_equal "$old_primary_ip:$old_primary_port" "new_primary_ip:new_primary_port"
    }
} my_slot_allocation cluster_allocate_replicas ;# start_cluster
