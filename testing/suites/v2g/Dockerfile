# The V2G SIL: EVerest's demo image, pinned, plus the broker, the capture
# tools and pytest. EVerest 2025.10.0 (everest-demo tag 2025.10.0-single-phase).
FROM ghcr.io/everest/everest-demo/manager@sha256:9165d0937323ae61ee2e496f68f3c4a465ed9099740e489df1f0161265492756

RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      mosquitto mosquitto-clients iproute2 tshark \
 && rm -rf /var/lib/apt/lists/* \
 && pip install --no-cache-dir --break-system-packages pytest==9.1.1

# EVerest's test PKI, where its modules look: Evse15118D20 and Josev's
# ISO 15118-20 states read the V2G certificates even without TLS.
RUN cd /ext/source/tests/ocpp_tests/test_sets/everest-aux && ./install_certs.sh /ext/dist

# Josev's TLS 1.3 client presents a vehicle certificate, which that PKI lacks:
# one leaf, signed by its V2G root.
RUN cd /ext/dist/etc/everest/certs && mkdir client/vehicle && pw=$(cat client/v2g/V2G_ROOT_CA_PASSWORD.txt) \
 && openssl ecparam -genkey -name prime256v1 | openssl ec -aes-128-cbc -passout pass:$pw -out client/vehicle/VEHICLE_LEAF.key \
 && openssl req -new -key client/vehicle/VEHICLE_LEAF.key -passin pass:$pw -subj "/CN=SIL vehicle" -out /tmp/vehicle.csr \
 && printf 'basicConstraints=critical,CA:false\nkeyUsage=critical,digitalSignature\n' > /tmp/vehicle.ext \
 && openssl x509 -req -in /tmp/vehicle.csr -extfile /tmp/vehicle.ext -CA ca/v2g/V2G_ROOT_CA.pem -CAkey client/v2g/V2G_ROOT_CA.key \
      -passin pass:$pw -set_serial 1 -days 3650 -out client/vehicle/VEHICLE_CERT_CHAIN.pem \
 && echo $pw > client/vehicle/VEHICLE_LEAF_PASSWORD.txt && rm /tmp/vehicle.*

WORKDIR /suite
ENTRYPOINT ["python3", "-m", "pytest", "-p", "no:cacheprovider"]
