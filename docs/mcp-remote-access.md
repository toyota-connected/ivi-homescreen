# Reaching the MCP surface from off the box

The shell serves MCP on a Unix domain socket and nothing else. There is no
TCP listener, no TLS, and no certificate configuration, and this is a
deliberate boundary rather than an unfinished one.

Off-box access is supported by putting a TLS terminator in front of that
socket. This document says why, and how.

## Why the shell has no TLS

The embedder has to own the parts only it can: the semantics tree arrives
through the Flutter embedder callback, actions and synthesized pointer events
go back through the embedder API, and the policy that governs them -- the
build flag, the runtime `enable_mcp`, the per-consumer action mask -- is
platform policy that an application must not be able to grant itself.

Transport security is not in that set. Certificate provisioning, CA trust,
rotation, cipher selection and revocation are product and fleet decisions with
their own lifecycle, and putting them in the shell would mean a UI shell
carrying a crypto dependency, a trust model, and a set of policy knobs that
change on a different schedule from the shell itself.

So the shell keeps the one control it can enforce better than anything else
can: the socket is created under a `0177` umask in a `0700` directory, and
every connection is checked with `SO_PEERCRED` before a byte is read. That is
a kernel-attested peer uid, which is a stronger statement than any token.

## The shape

```
  agent  ──mTLS over TCP/vsock──▶  terminator  ──▶  /run/user/<uid>/ivi-homescreen/mcp.sock  ──▶  shell
         (client certificate)      (stunnel,                (0600, SO_PEERCRED)
                                    ghostunnel)
```

The terminator authenticates the client with a certificate and forwards plain
HTTP into the Unix socket. Certificates, CA and rotation live entirely in that
unit's configuration.

## Two things you must get right

**The terminator has to run as the same user as the shell.** `SO_PEERCRED`
checks the connecting process's uid, and the connecting process is now the
terminator, not the agent. Running it as another user means every connection
is refused; running it as root means it is refused too.

**After that, the certificate check is the only thing standing in front of the
UI.** Once a connection reaches the socket it carries the shell user's full
authority over the drive surface, because peer credentials can no longer
distinguish the agent from the terminator. Require a client certificate --
`verifyPeer`, not `verifyChain` alone -- and restrict which certificates are
accepted rather than trusting a whole CA.

## stunnel

Packaged nearly everywhere, including Yocto, so this is usually the path of
least resistance.

```ini
[mcp]
accept  = 0.0.0.0:8443
connect = /run/user/1000/ivi-homescreen/mcp.sock
cert    = /etc/ivi/mcp/server.pem
key     = /etc/ivi/mcp/server.key
CAfile  = /etc/ivi/mcp/agents-ca.pem

; Require a client certificate that chains to CAfile. Without this, TLS
; authenticates the server to the agent and nothing in the other direction,
; which is not what this is for.
verifyPeer = yes
sslVersionMin = TLSv1.3
```

Run it as the shell's user:

```ini
setuid = 1000
setgid = 1000
```

## ghostunnel

Purpose-built for terminating mTLS in front of a Unix socket, and it can
authorize on certificate fields rather than accepting any certificate the CA
issued -- which is the difference between "an agent" and "this agent".

```sh
ghostunnel server \
  --listen   0.0.0.0:8443 \
  --target   unix:/run/user/1000/ivi-homescreen/mcp.sock \
  --cert     /etc/ivi/mcp/server.pem \
  --key      /etc/ivi/mcp/server.key \
  --cacert   /etc/ivi/mcp/agents-ca.pem \
  --allow-cn drive-agent
```

## vsock

For an agent in another VM on the same machine, a vsock terminator avoids the
network entirely: the channel is CID-scoped and never reaches an interface.
socat can bridge it, and mTLS on top is still worth having if the guest is not
fully trusted:

```sh
socat VSOCK-LISTEN:8443,fork,reuseaddr \
      UNIX-CONNECT:/run/user/1000/ivi-homescreen/mcp.sock
```

## What the shell still checks

The transport refuses any request carrying an `Origin` header, on both the
request and event-stream paths.

This costs nothing while the only listener is a Unix socket -- no browser can
open one. It matters as soon as a terminator is in front, because a terminator
forwards plain HTTP and a rebound browser request arrives looking like an
ordinary local one. Only a browser sets `Origin`, and no browser is a
legitimate client here, so its presence is enough to refuse on. The MCP
specification requires HTTP transports to validate it for this reason.

Do not configure the terminator to add or rewrite request headers. Nothing in
the transport trusts `X-Forwarded-*`, and adding an `Origin` will get every
request refused.

---

For what the surface exposes once a client reaches it, what bounds it, and the
decisions an integrator still has to make, see
[`mcp-security.md`](mcp-security.md).
