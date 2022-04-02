# tcpbwd
tcpbwd is TCP backward.

用于反向桥接TCP连接。类似于花生壳的内网端口映射功能。

例如你有一台有公网IP的服务器HOST_A，和一台没有公网IP但能访问公网的PC_A。

但是你希望通过公网访问PC_A。 这时TCP backward就可以发挥用处了。

step1: 在HOST_A上运行 tcpbwd server <CTRL_PORT>        <PUBLISH_PORT>

setp2: 在PC_A上运行   tcpbwd client <HOST_A:CTRL_PORT> <PC_A:PORT>

这样就可以通过直接访问HOST_A:PUBLISH_PORT来间接的访问PC_A:PORT了

