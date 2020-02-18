# HomeShare - Copy files between computers on the home network

Copying files between computers on the home LAN is still complicated today.
For example, using a USB flash drive requires 2 copies (to and from the drive),
and uploading to a cloud storage service (e.g., Dropbox or Google Drive) is slow
because the average home Internet upload is much slower than download.

With HomeShare, just run an instance on each computer in the network. They will
automatically discover the other instances, and allow securely copying files directly
between computers (no Internet connection required) using an existing Wi-Fi or
Ethernet cable connection.

## Download

Download `HomeShare.exe` from the [Releases](https://github.com/alandau/HomeShare/releases) page.

## Distinctive Features

- Automatically discover other computers running HomeShare on the network.
- Copy files over either a wireless (Wi-Fi) or wired (Ethernet) network.
- Supports direct Ethernet cable connection between two computers for higher speed than Wi-Fi, no configuration required.
- All transfers are authenticated and encrypted (X25519 key exchange, Ed25519 signatures, ChaCha20-Poly1305 AEAD encryption).

## System Requirements

Windows XP SP3 and up, including Windows 7 and Windows 10 (32- or 64-bit).

## License

MIT
