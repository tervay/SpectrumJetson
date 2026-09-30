// A stand-in for robot code: a NetworkTables server that publishes the Driver Station's control
// word (/FMSInfo/FMSControlData) through a list of phases, so PhotonVision's robot-state features
// (idle while disabled, photonvision-49) can be tested on the bench.
// Run ON THE JETSON, reached by PhotonVision as its robot (see tests/fake-robot/run.sh):
//   java -cp /opt/photonvision/photonvision.jar FakeRobot.java disabled:30 enabled:30 disabled:30
// Each phase is STATE:SECONDS, STATE one of disabled, enabled, auto, and any of those with "fms-"
// in front (FMS attached). Prints "phase <n> <state> <unix ms>" as each one starts. Touching
// /tmp/fake-robot-stop ends the run early.
import edu.wpi.first.networktables.NetworkTableInstance;
import org.photonvision.jni.LibraryLoader;

public class FakeRobot {
    static long word(String state) {
        long w = 32; // DS attached
        if (state.startsWith("fms-")) {
            w |= 16;
            state = state.substring(4);
        }
        switch (state) {
            case "disabled" -> {}
            case "enabled" -> w |= 1;
            case "auto" -> w |= 1 | 2;
            default -> throw new IllegalArgumentException("Unknown state " + state);
        }
        return w;
    }

    // Touch this file to end the run early (tests/ui's idle test does, over SSH).
    static final java.io.File STOP = new java.io.File("/tmp/fake-robot-stop");

    public static void main(String[] args) throws Exception {
        STOP.delete();
        LibraryLoader.loadWpiLibraries();
        var nt = NetworkTableInstance.create();
        nt.startServer("/tmp/fake-robot-nt.json");
        var fms = nt.getTable("FMSInfo");
        var control = fms.getIntegerTopic("FMSControlData").publish();
        control.set(word("disabled"));
        System.out.println("waiting for PhotonVision to connect");
        long giveUp = System.currentTimeMillis() + 30_000;
        while (nt.getConnections().length == 0) {
            if (System.currentTimeMillis() > giveUp) {
                System.out.println("TIMEOUT: PhotonVision didn't connect within 30 s");
                System.exit(2);
            }
            Thread.sleep(100);
        }
        System.out.println("connected " + System.currentTimeMillis());
        Thread.sleep(2000);
        for (int i = 0; i < args.length; i++) {
            String[] p = args[i].split(":");
            control.set(word(p[0]));
            nt.flush();
            System.out.println("phase " + i + " " + p[0] + " " + System.currentTimeMillis());
            long until = System.currentTimeMillis() + (long) (Double.parseDouble(p[1]) * 1000);
            while (System.currentTimeMillis() < until) {
                if (STOP.exists()) {
                    System.out.println("stopped early " + System.currentTimeMillis());
                    i = args.length;
                    break;
                }
                Thread.sleep(100);
            }
        }
        System.out.println("done " + System.currentTimeMillis());
        nt.close();
    }
}
