package com.anlikklip.mobile;

import org.json.JSONObject;

import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.InterfaceAddress;
import java.net.NetworkInterface;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Enumeration;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

public class ServerDiscovery {
    public static final int DISCOVERY_PORT = 47821;
    private static final byte[] DISCOVER_PAYLOAD = "ANLIKKLIP_DISCOVER".getBytes(StandardCharsets.US_ASCII);

    public static class DiscoveredPc {
        public String name;
        public String ip;
        public int port;

        @Override
        public String toString() {
            return name + " (" + ip + ":" + port + ")";
        }
    }

    public static List<DiscoveredPc> discover(int timeoutMs) {
        Map<String, DiscoveredPc> found = new LinkedHashMap<>();
        try (DatagramSocket socket = new DatagramSocket()) {
            socket.setBroadcast(true);
            socket.setSoTimeout(350);

            sendBroadcastPackets(socket);

            long deadline = System.currentTimeMillis() + timeoutMs;
            byte[] buf = new byte[1024];
            while (System.currentTimeMillis() < deadline) {
                try {
                    DatagramPacket pkt = new DatagramPacket(buf, buf.length);
                    socket.receive(pkt);
                    String msg = new String(pkt.getData(), 0, pkt.getLength(), StandardCharsets.UTF_8).trim();
                    JSONObject obj = new JSONObject(msg);
                    if ("AnlikKlip".equals(obj.optString("app"))) {
                        DiscoveredPc pc = new DiscoveredPc();
                        pc.name = obj.optString("name", "PC");
                        pc.ip = obj.optString("ip", pkt.getAddress().getHostAddress());
                        pc.port = obj.optInt("port", 47820);
                        found.put(pc.ip + ":" + pc.port, pc);
                    }
                } catch (Exception ignored) {
                    // Wait until deadline
                }
            }
        } catch (Exception ignored) {
        }
        return new ArrayList<>(found.values());
    }

    private static void sendBroadcastPackets(DatagramSocket socket) {
        try {
            socket.send(new DatagramPacket(
                    DISCOVER_PAYLOAD, DISCOVER_PAYLOAD.length,
                    InetAddress.getByName("255.255.255.255"), DISCOVERY_PORT));
        } catch (Exception ignored) {
        }
        try {
            Enumeration<NetworkInterface> interfaces = NetworkInterface.getNetworkInterfaces();
            while (interfaces != null && interfaces.hasMoreElements()) {
                NetworkInterface nif = interfaces.nextElement();
                if (nif.isLoopback() || !nif.isUp()) continue;
                for (InterfaceAddress ia : nif.getInterfaceAddresses()) {
                    InetAddress bcast = ia.getBroadcast();
                    if (bcast != null) {
                        try {
                            socket.send(new DatagramPacket(
                                    DISCOVER_PAYLOAD, DISCOVER_PAYLOAD.length,
                                    bcast, DISCOVERY_PORT));
                        } catch (Exception ignored) {
                        }
                    }
                }
            }
        } catch (Exception ignored) {
        }
    }
}
