# Security

## Reporting a vulnerability

Please report it privately, through GitHub's **Report a vulnerability** button on this repository's Security tab, not in a public issue. Include the Kosh commit, how to reproduce it, and what an attacker gains.

You can expect an acknowledgement within a week. Kosh is maintained by one person, so a fix may take longer; you will hear how it is going, and you will be credited in the fix unless you prefer not to be.

Kosh has no releases yet; fixes go to the `main` branch.

## What Kosh defends against

| Threat | Defence |
|---|---|
| A server started with no options, reachable from the network | It listens on 127.0.0.1 unless `--bind` says otherwise. |
| Listening on every interface without a password | Protected mode (on by default) refuses clients on other machines with `-DENIED`. |
| Clients that shouldn't have access | `--requirepass-file`: every command but `AUTH` and `QUIT` gets `-NOAUTH` until the client authenticates. |
| A client without the password making the server buffer data | Before `AUTH`, a command may have at most 10 arguments of 16 KB each, as in Redis. |
| Guessing the password from response times | The comparison takes the same time however much of a guess is right ([measured](bench/results/password-check/README.md)). |
| The password leaking | It is never logged, never written to the AOF, and never quoted in an error. `--requirepass-file` keeps it out of the process list. |
| Keys chosen so they all land in one bucket (hash flooding) | Keys are hashed with SipHash-1-3 under a random key per process. |
| Malformed or hostile protocol input | Strict RESP parsing with Redis's limits (512 MB bulk strings, 64 KB inline commands, 1 GB per incomplete command), and a fuzz target ([`tests/fuzz`](tests/fuzz)) run under AddressSanitizer and UndefinedBehaviorSanitizer. |
| One client starving the others | Output to a slow client is bounded, so the server stops reading from it instead of buffering without limit; `--maxclients` caps connections. |
| Another program taking over the port (Windows) | The listening socket is bound with `SO_EXCLUSIVEADDRUSE`. |

## What it doesn't defend against

- **Eavesdropping.** There is no TLS. Commands, data and the password cross the network in clear text. Use Kosh on a trusted network, or behind a TLS tunnel such as stunnel or spiped.
- **Guessing the password by trying many.** There is no limit on failed `AUTH` attempts (Redis has none either). Use a long random password, for example `openssl rand -hex 32`.
- **Authenticated clients.** There is one password and no per-user permissions: a client that has it can read, change and delete every key, and run `BGREWRITEAOF`, which blocks the server while it runs.
- **Access to the machine.** The AOF holds every key in plain text; protect it, and the password file, with file permissions. Another user on the machine can read `--requirepass` from the process list.
- **IPv6.** Kosh listens on IPv4 only.
