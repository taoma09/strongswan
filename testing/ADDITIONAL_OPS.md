# libs
```
apt-get install flex bison libssl-dev bindfs dnsmasq

```

# iptables
```
# @moon
ip r add ${HOST_SUBNET} via 192.168.1.254 dev eth2
iptables -A INPUT -p icmp -j ACCEPT
iptables -A OUTPUT -p icmp -j ACCEPT

# @host
iptables -t nat -A POSTROUTING -s 192.168.1.0/24 -o eth0 -j SNAT --to ${HOST_IP}

```

