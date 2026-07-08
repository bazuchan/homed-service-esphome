FROM    127.0.0.1:5000/homed-service:latest
ARG     TARGETARCH
RUN     mkdir /etc/homed
RUN     ln -s /data/homed-esphome.conf /etc/homed/homed-esphome.conf
RUN     ln -s /data/homed.log /var/log/homed.log
COPY    homed-esphome-$TARGETARCH /usr/bin/homed-esphome
CMD     ["/usr/bin/homed-esphome"]
