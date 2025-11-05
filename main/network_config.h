#ifndef NETWORK_CONFIG_H
#define NETWORK_CONFIG_H

// Static IP Configuration
// Change these values according to your network setup

// ESP32 Static IP Address (individual octets)
#define STATIC_IP_OCTET1   192
#define STATIC_IP_OCTET2   168
#define STATIC_IP_OCTET3   1
#define STATIC_IP_OCTET4   100

// Gateway IP (usually your router's IP)
#define GATEWAY_OCTET1     192
#define GATEWAY_OCTET2     168
#define GATEWAY_OCTET3     1
#define GATEWAY_OCTET4     1

// Subnet Mask (usually 255.255.255.0 for home networks)
#define NETMASK_OCTET1     255
#define NETMASK_OCTET2     255
#define NETMASK_OCTET3     255
#define NETMASK_OCTET4     0

// DNS Servers
#define PRIMARY_DNS_OCTET1    8
#define PRIMARY_DNS_OCTET2    8
#define PRIMARY_DNS_OCTET3    8
#define PRIMARY_DNS_OCTET4    8

#define SECONDARY_DNS_OCTET1  8
#define SECONDARY_DNS_OCTET2  8
#define SECONDARY_DNS_OCTET3  4
#define SECONDARY_DNS_OCTET4  4

// Alternative DNS options you can use:
// Cloudflare DNS:   1.1.1.1 and 1.0.0.1  
// OpenDNS:          208.67.222.222 and 208.67.220.220
// Your ISP DNS:     Check with your internet provider

/*
 * IMPORTANT NETWORK SETUP NOTES:
 * 
 * 1. Make sure the static IP (192.168.1.100) is OUTSIDE your router's DHCP range
 *    - Most routers use DHCP range like 192.168.1.10 to 192.168.1.50
 *    - So 192.168.1.100 should be safe
 *    - Check your router's admin panel to confirm DHCP range
 * 
 * 2. Make sure the gateway IP matches your router's IP
 *    - Common router IPs: 192.168.1.1, 192.168.0.1, 10.0.0.1
 *    - Find yours by running "ipconfig" on Windows or checking router label
 * 
 * 3. If your network uses different IP range (like 192.168.0.x), update accordingly:
 *    Change STATIC_IP_OCTET3 and GATEWAY_OCTET3 from 1 to 0
 * 
 * 4. For corporate networks, you may need to ask IT for:
 *    - Available static IP range
 *    - Gateway IP
 *    - DNS server addresses
 */

#endif // NETWORK_CONFIG_H