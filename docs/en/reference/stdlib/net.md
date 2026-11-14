# std.net

Nonblocking UDP networking.

## Import

```lhat
import^std.net
```

## Example

<<< @/../sample/docs/stdlib/net.lh{lhat}

Run from the repository root:

```sh
lhat sample/docs/stdlib/net.lh
```

## Usage and rules

Send and receive using addresses and ports. Receiving never waits; the first tuple item is `nil^` when no data is available.

The `Bytes` overloads also require `std.binary` registration.

## Types

### Error

Each variant is an error type. See [Errors](../errors.md) for common members and handling rules.

- `Error.AddressInUse`
- `Error.Resolve`
- `Error.Unreachable`
- `Error.TooLarge`
- `Error.Closed`
- `Error.Failed`

### Udp

A host-exposed type, created and used through the APIs below.

## Functions

Signatures are generated from CLI registration data. See [Standard library](./index.md) for notation.

### udp

Create a UDP socket.

```text
p^ -> std.net.Udp|std.net.Error;
```

## Methods and operators

Signatures are generated from CLI registration data. See [Standard library](./index.md) for notation.

### Udp.bind

Bind to a local address and port.

```text
p^self^, string^, number^ -> nil^|std.net.Error;
```

### Udp.setPeer

Set the default destination address and port.

```text
p^self^, string^, number^ -> nil^|std.net.Error;
```

### Udp.send

Send data to the default peer.

```text
p^self^, string^ -> nil^|std.net.Error;
p^self^, std.binary.Bytes -> nil^|std.net.Error;
```

### Udp.sendTo

Send using `(data, address, port)`.

```text
p^self^, string^, string^, number^ -> nil^|std.net.Error;
p^self^, std.binary.Bytes, string^, number^ -> nil^|std.net.Error;
```

### Udp.receive

Receive a tuple of data, sender address, and port.

```text
p^self^ -> (string^|nil^, string^, number^)|std.net.Error;
```

### Udp.getLocal

Get the local address and port.

```text
p^self^ -> string^, number^;
```

### Udp.setBroadcast

Enable or disable broadcast sending.

```text
p^self^, bool^ -> nil^|std.net.Error;
```

### Udp.dispose

Release the reference to the resource.

```text
p^self^;
```

### Udp.receiveInto

Receive into an existing buffer, returning the buffer, sender, and port.

```text
p^self^, std.binary.Bytes -> (std.binary.Bytes|nil^, string^, number^)|std.net.Error;
```

## Implementation and registration

[net.h](https://github.com/SAM-tak/lhat/blob/main/stdlib/net.h) / [net.c](https://github.com/SAM-tak/lhat/blob/main/stdlib/net.c)
