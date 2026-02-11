# Windows TCP Port Forwarder

A high-performance TCP port forwarder for Windows with optional IP whitelisting and verbose logging capabilities.

## Features

- ✅ Forward TCP traffic from a local port to any remote host:port
- ✅ Optional IP whitelisting for enhanced security
- ✅ Supports up to 100 concurrent connections
- ✅ Bidirectional data forwarding with real-time statistics
- ✅ Verbose mode for debugging rejected connections
- ✅ Aggressive TCP keepalive settings for dead connection detection
- ✅ Low-latency forwarding (Nagle's algorithm disabled)
- ✅ Graceful shutdown handling (Ctrl+C)
- ✅ Thread-safe connection management

## Requirements

- Windows operating system
- Visual Studio (for compilation) or MinGW-w64
- Winsock2 library (included with Windows SDK)

## Compilation

### Using Visual Studio Developer Command Prompt:

```cmd
cl port_forwarder.c /Fe:port_forwarder.exe ws2_32.lib
```

### Using MinGW-w64:

```cmd
gcc port_forwarder.c -o port_forwarder.exe -lws2_32
```

## Usage

```
port_forwarder.exe <local_port> <remote_host> <remote_port> [allowed_ip] [-v]
```

### Parameters

- `<local_port>` - Local port to listen on (1-65535)
- `<remote_host>` - Remote hostname or IP address to forward traffic to
- `<remote_port>` - Remote port to forward traffic to (1-65535)
- `[allowed_ip]` - *Optional* - Only accept connections from this IP address
- `[-v]` - *Optional* - Enable verbose mode (show rejected connections)

### Examples

#### Basic port forwarding (no IP filtering)
Forward local port 8080 to remote server 192.168.1.100 port 80:
```cmd
port_forwarder.exe 8080 192.168.1.100 80
```

#### Port forwarding with IP whitelisting
Only accept connections from 192.168.1.50:
```cmd
port_forwarder.exe 8080 192.168.1.100 80 192.168.1.50
```

#### Port forwarding with IP whitelisting and verbose logging
Show rejected connection attempts:
```cmd
port_forwarder.exe 8080 192.168.1.100 80 192.168.1.50 -v
```

#### Forward to remote hostname
```cmd
port_forwarder.exe 3306 database.example.com 3306
```

#### SSH port forwarding with IP restriction
```cmd
port_forwarder.exe 2222 10.0.0.50 22 192.168.1.100
```

## Use Cases

### Local Development
- Access services running in containers or VMs
- Forward ports from WSL2 to Windows host
- Test webhook endpoints locally

### Network Bridging
- Bridge different network segments
- Bypass firewall restrictions (authorized use only)
- Access internal services from DMZ

### Database Access
- Secure database connections with IP whitelisting
- Tunnel database traffic through jump hosts
- Expose local databases for remote development

### Security Testing
- Monitor and analyze TCP traffic patterns
- Test IP-based access controls
- Audit connection attempts with verbose mode

## Output Information

The forwarder provides detailed logging:

```
[INFO] Configuration:
  Local port:  8080
  Remote host: 192.168.1.100
  Remote port: 80
  Allowed IP:  192.168.1.50 (filtered mode)
  Verbose:     OFF

[INFO] Listening on port 8080...
[INFO] Press Ctrl+C to stop

[INFO] New connection from 192.168.1.50:54321 ACCEPTED
[INFO] Connected to remote 192.168.1.100:80
[INFO] Connection established, forwarding traffic...
[INFO] Closing connection (Sent: 1024 bytes, Received: 2048 bytes, Total: 3072 bytes)
```

### Verbose Mode Output

When `-v` flag is enabled, rejected connections are also logged:

```
[INFO] Connection from 192.168.1.99:54322 REJECTED (IP not allowed)
```

## Technical Details

### Performance Optimizations

- **Buffer Size**: 8KB buffers for efficient data transfer
- **TCP_NODELAY**: Disabled Nagle's algorithm for lower latency
- **Keepalive**: Aggressive settings (10s initial, 1s interval) to detect dead connections
- **Thread Pool**: Up to 100 concurrent connections with dedicated forwarding threads
- **Non-blocking Accept**: Timeout-based select() for responsive shutdown

### Connection Handling

Each connection spawns a dedicated forwarding thread that:
1. Establishes connection to remote host
2. Sets socket options (timeouts, keepalive, TCP_NODELAY)
3. Uses select() for bidirectional forwarding
4. Tracks bytes transferred in each direction
5. Gracefully closes both sockets on termination

### Error Handling

The forwarder handles common network errors gracefully:
- `WSAECONNRESET` - Connection reset by peer
- `WSAECONNABORTED` - Connection aborted
- `WSAENETRESET` - Network caused disconnect
- `WSAETIMEDOUT` - Connection timed out

### Thread Safety

- Critical sections protect the connection array
- Volatile `running` flag for clean shutdown
- Thread synchronization during cleanup

## Security Considerations

⚠️ **Important Security Notes:**

1. **IP Whitelisting**: Use the `[allowed_ip]` parameter in production to restrict access
2. **No Encryption**: This forwarder does not encrypt traffic. Use with SSH tunnels or VPNs for sensitive data
3. **Authentication**: No built-in authentication. Ensure the remote service has proper security
4. **Firewall**: Configure Windows Firewall to restrict access to the listening port
5. **Logging**: Disable verbose mode in production to avoid log file bloat
6. **Resource Limits**: 100 connection limit prevents resource exhaustion

## Troubleshooting

### Port Already in Use
```
[ERROR] bind() failed: 10048
```
**Solution**: Another application is using the port. Choose a different local port or stop the conflicting service.

### Cannot Connect to Remote
```
[ERROR] connect() to remote failed: 10061
```
**Solution**: Verify the remote host is reachable and the service is running on the specified port.

### Permission Denied
```
[ERROR] bind() failed: 10013
```
**Solution**: Ports below 1024 may require administrator privileges. Run as Administrator or use a higher port number.

### Connection Rejected
```
[INFO] Connection from X.X.X.X:XXXXX REJECTED (IP not allowed)
```
**Solution**: Enable verbose mode (`-v`) to see rejected IPs, or remove IP filtering.

## Stopping the Forwarder

Press `Ctrl+C` to gracefully stop the forwarder. It will:
1. Stop accepting new connections
2. Wait for active connections to complete (up to 5 seconds)
3. Close all sockets
4. Clean up resources
5. Display cleanup confirmation

## Limitations

- **Windows Only**: Uses Winsock2 API (not portable to Linux/macOS)
- **IPv4 Only**: Currently supports IPv4 addresses only
- **Single IP Filter**: Only one allowed IP address can be specified
- **No Load Balancing**: Traffic is forwarded to a single remote endpoint
- **No Traffic Inspection**: Raw TCP forwarding without content analysis
- **No Bandwidth Limiting**: No built-in throttling or QoS

## Future Enhancements

Potential features for future versions:
- [ ] IPv6 support
- [ ] Multiple IP whitelist/blacklist
- [ ] SSL/TLS encryption wrapper
- [ ] Traffic statistics dashboard
- [ ] Configuration file support
- [ ] Bandwidth limiting
- [ ] Round-robin load balancing
- [ ] Connection rate limiting
- [ ] Cross-platform support (Linux/macOS)

## License

This software is provided as-is for educational and legitimate network administration purposes. Use responsibly and in compliance with applicable laws and regulations.

## Contributing

Contributions are welcome! Please ensure:
- Code follows the existing style
- Changes are well-tested on Windows
- Comments explain complex logic
- README is updated for new features

## Support

For issues, questions, or feature requests, please open an issue on the project repository.

You can also support my work on [Patreon](https://www.patreon.com/Lsecqt)

---