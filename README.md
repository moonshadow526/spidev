# spidev
rk平台上使用spi和mcu通信传输文件
## rk平台上配置spi设备树,例如：
&spi1 {
        status = "okay";
        pinctrl-names = "default";
        mcu@0 {
                compatible = "rockchip,spidev";
                reg = <0>;  // chip select 0
                spi-max-frequency = <15000000>; // 15MHz
        };
};
## 配置cs-gpio在传输数据包时代码控制gpio的高低用于触发mcu外部中断开启DMA接收，读取ACK时置高；防止数据包和通信包出现混乱
直接使用export模式导出gpio
echo 59 > /sys/class/gpio/export
echo out > /sys/class/gpio/gpio59/direction 
## 发生数据代码见spidev_opt.c

