// Stands in for robot code reading PhotonVision's results: a NetworkTables server (reached as the
// robot, see tests/fake-robot/run.sh) that subscribes to every camera's result topic and measures
// the capture timestamps the robot would fuse: frame-to-frame interval and its jitter. It says the
// robot is enabled, so the cameras run at full rate.
//   java -cp /opt/photonvision/photonvision.jar ResultTimestamps.java [seconds]
// A result starts with its metadata (little-endian int64s): sequence id, capture timestamp (us),
// publish timestamp (us), time since last pong.
import edu.wpi.first.networktables.NetworkTableInstance;
import edu.wpi.first.networktables.PubSubOption;
import edu.wpi.first.networktables.RawSubscriber;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.*;
import org.photonvision.jni.LibraryLoader;

public class ResultTimestamps {
    public static void main(String[] args) throws Exception {
        double seconds = args.length > 0 ? Double.parseDouble(args[0]) : 30;
        LibraryLoader.loadWpiLibraries();
        var nt = NetworkTableInstance.create();
        nt.startServer("/tmp/fake-robot-results-nt.json");
        // Enabled, DS attached: full rate, as in a match (disabled would idle at 30 fps, -49).
        var control = nt.getTable("FMSInfo").getIntegerTopic("FMSControlData").publish();
        control.set(32 | 1);
        long giveUp = System.currentTimeMillis() + 30_000;
        while (nt.getConnections().length == 0) {
            if (System.currentTimeMillis() > giveUp) {
                System.out.println("TIMEOUT: PhotonVision didn't connect within 30 s");
                System.exit(2);
            }
            Thread.sleep(100);
        }
        // Topics are only announced to a server that subscribes: ask for everything first.
        var all = new edu.wpi.first.networktables.MultiSubscriber(nt, new String[] {"/photonvision/"}, PubSubOption.topicsOnly(true));
        Thread.sleep(3000); // let PhotonVision publish its topics
        var subs = new TreeMap<String, RawSubscriber>();
        for (var topic : nt.getTopics("/photonvision/")) {
            String name = topic.getName();
            if (!name.endsWith("/rawBytes")) continue;
            String camera = name.substring("/photonvision/".length(), name.length() - "/rawBytes".length());
            // Subscribe with PhotonVision's own type string (photonstruct:...), or nothing arrives.
            subs.put(camera, nt.getRawTopic(name).subscribe(topic.getTypeString(), new byte[0], PubSubOption.sendAll(true), PubSubOption.pollStorage(512)));
        }
        System.out.println("cameras: " + subs.keySet());
        if (subs.isEmpty()) for (var topic : nt.getTopics("/photonvision/")) System.out.println("  topic " + topic.getName());
        var stamps = new HashMap<String, List<Long>>();
        long end = System.currentTimeMillis() + (long) (seconds * 1000);
        while (System.currentTimeMillis() < end) {
            for (var e : subs.entrySet()) {
                for (var v : e.getValue().readQueue()) {
                    if (v.value.length < 16) continue;
                    long capture = ByteBuffer.wrap(v.value).order(ByteOrder.LITTLE_ENDIAN).getLong(8);
                    stamps.computeIfAbsent(e.getKey(), k -> new ArrayList<>()).add(capture);
                }
            }
            Thread.sleep(20);
        }
        for (var e : new TreeMap<>(stamps).entrySet()) {
            var t = e.getValue();
            var d = new ArrayList<Double>();
            for (int i = 1; i < t.size(); i++) {
                double ms = (t.get(i) - t.get(i - 1)) / 1000.0;
                if (ms > 0 && ms < 30) d.add(ms); // a skipped frame isn't jitter
            }
            if (d.size() < 10) continue;
            double mean = d.stream().mapToDouble(x -> x).average().orElse(0);
            double sd = Math.sqrt(d.stream().mapToDouble(x -> (x - mean) * (x - mean)).average().orElse(0));
            double min = Collections.min(d), max = Collections.max(d);
            System.out.printf("%-12s %5d results, interval %.3f ms, jitter %.3f ms, min %.2f, max %.2f%n", e.getKey(), t.size(), mean, sd, min, max);
        }
        nt.close();
    }
}
