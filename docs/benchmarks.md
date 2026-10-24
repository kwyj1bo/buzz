# Buzz benchmark results

- Date: 2026-10-24T19:04Z
- Machine: 2 vCPU (1 cores x 2 threads), AMD EPYC 7763 64-Core Processor
- Memory: 7 GiB, kernel 6.17.0-1022-azure
- Build: Release, c++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0
- Server and load generator run on the same machine over loopback, with TLS 1.3.

| Metric | Result |
| --- | --- |
| TLS handshakes per second (2,000 clients) | 1127.42 /s |
| TLS handshake p50 | 6891.91 µs |
| TLS handshake p99 | 10667.77 µs |
| Concurrent logged-in connections | 10000.00  |
| Server memory with all connections open | 302.59 MiB |
| Server memory per connection | 24.54 KiB |
| Ping round trip p50 with 10k connections open | 71.43 µs |
| Ping round trip p99 with 10k connections open | 88.47 µs |
| Max chat throughput, messages in (1,000 clients, 50 rooms) | 2349.10 /s |
| Max chat throughput, deliveries out (fan-out x20) | 46982.00 /s |
| Latency p50 at max throughput | 4426.55 µs |
| Latency p99 at max throughput | 406331.48 µs |
| Steady load: messages in | 999.70 /s |
| Steady load: deliveries out | 19994.00 /s |
| Steady load: end-to-end latency p50 | 437.23 µs |
| Steady load: end-to-end latency p99 | 3122.76 µs |
| Steady load: server CPU | 33.00 % |
| Heavy load: deliveries out | 39996.00 /s |
| Heavy load: end-to-end latency p50 | 759.53 µs |
| Heavy load: end-to-end latency p99 | 5349.56 µs |
| Heavy load: server CPU | 57.17 % |
| Replay of 1,000 missed messages on rejoin | 12.31 ms |
| Logins per second, 1 worker (PBKDF2 600k) | 5.72 /s |
| Logins per second, 2 workers (PBKDF2 600k) | 6.48 /s |
| Ping p50 during the login storm | 59.02 µs |
| Ping p99 during the login storm | 1286.41 µs |
| Server CPU cores busy during the login storm | 1.98  |
| P2P encrypted UDP round trip p50 | 62.57 µs |
| P2P encrypted UDP round trips per second | 16173.61 /s |

<details><summary>All raw results</summary>

```text
handshakes handshakes_per_sec 1127.42 1/s
handshakes handshake_p50 6891.91 us
handshakes handshake_p95 9057.25 us
handshakes handshake_p99 10667.77 us
handshakes handshake_max 14510.16 us
connections connections 10000.00 
connections connect_login_per_sec 635.83 1/s
connections server_rss_mib 302.59 MiB
connections rss_per_connection_kib 24.54 KiB
connections ping_rtt_loaded_p50 71.43 us
connections ping_rtt_loaded_p95 74.31 us
connections ping_rtt_loaded_p99 88.47 us
connections ping_rtt_loaded_max 111.94 us
chat_max chat_clients 1000.00 
chat_max chat_setup_s 1.85 s
chat_max chat_messages_per_sec 2349.10 1/s
chat_max chat_deliveries_per_sec 46982.00 1/s
chat_max server_cpu_percent 84.00 %
chat_max chat_latency_p50 4426.55 us
chat_max chat_latency_p95 363678.43 us
chat_max chat_latency_p99 406331.48 us
chat_max chat_latency_max 570335.46 us
chat_steady chat_clients 1000.00 
chat_steady chat_setup_s 1.79 s
chat_steady chat_messages_per_sec 999.70 1/s
chat_steady chat_deliveries_per_sec 19994.00 1/s
chat_steady server_cpu_percent 33.00 %
chat_steady chat_latency_p50 437.23 us
chat_steady chat_latency_p95 1180.06 us
chat_steady chat_latency_p99 3122.76 us
chat_steady chat_latency_max 9950.70 us
chat_heavy chat_clients 1000.00 
chat_heavy chat_setup_s 1.79 s
chat_heavy chat_messages_per_sec 1999.80 1/s
chat_heavy chat_deliveries_per_sec 39996.00 1/s
chat_heavy server_cpu_percent 57.17 %
chat_heavy chat_latency_p50 759.53 us
chat_heavy chat_latency_p95 2482.11 us
chat_heavy chat_latency_p99 5349.56 us
chat_heavy chat_latency_max 30088.06 us
sync replay_messages 1000.00 
sync replay_ms 12.31 ms
sync replay_msgs_per_sec 81249.65 1/s
logins_1_worker registrations_per_sec 5.66 1/s
logins_1_worker logins_per_sec 5.72 1/s
logins_1_worker login_p50 1397181.42 us
logins_1_worker login_p95 1399240.80 us
logins_1_worker login_p99 1399562.04 us
logins_1_worker login_max 1399576.35 us
logins_1_worker ping_rtt_during_logins_p50 58.28 us
logins_1_worker ping_rtt_during_logins_p95 68.44 us
logins_1_worker ping_rtt_during_logins_p99 78.16 us
logins_1_worker ping_rtt_during_logins_max 2098.55 us
logins_1_worker server_cpu_cores_busy 1.01 cores
logins_n_workers registrations_per_sec 6.46 1/s
logins_n_workers logins_per_sec 6.48 1/s
logins_n_workers login_p50 2456530.57 us
logins_n_workers login_p95 2481869.56 us
logins_n_workers login_p99 2483835.43 us
logins_n_workers login_max 2485886.06 us
logins_n_workers ping_rtt_during_logins_p50 59.02 us
logins_n_workers ping_rtt_during_logins_p95 72.99 us
logins_n_workers ping_rtt_during_logins_p99 1286.41 us
logins_n_workers ping_rtt_during_logins_max 2450.84 us
logins_n_workers server_cpu_cores_busy 1.98 cores
p2p p2p_roundtrips_per_sec 16173.61 1/s
p2p p2p_rtt_p50 62.57 us
p2p p2p_rtt_p95 68.38 us
p2p p2p_rtt_p99 79.80 us
p2p p2p_rtt_max 401.57 us
```

</details>
