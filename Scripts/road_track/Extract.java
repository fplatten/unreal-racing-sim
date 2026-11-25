import javax.imageio.ImageIO;
import java.awt.image.BufferedImage;
import java.io.*;
import java.util.*;

public class Extract {
    public static void main(String[] a) throws Exception {
        BufferedImage img = ImageIO.read(new File(a[0]));
        int w = img.getWidth(), h = img.getHeight();
        int cell = 5;
        Map<Long, double[]> cells = new HashMap<>();
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
            int c = img.getRGB(x, y);
            int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
            boolean red = r > 190 && g < 70 && b < 70;
            boolean yel = r > 200 && g > 160 && b < 80;
            boolean blu = r < 80 && g > 130 && b > 190;
            if (!(red || yel || blu)) continue;
            // exclude legend boxes / labels far from the track: handled later
            long k = ((long)(x / cell) << 32) | (y / cell);
            double[] s = cells.computeIfAbsent(k, kk -> new double[4]);
            s[0] += x; s[1] += y; s[2]++; s[3] = red ? 1 : yel ? 2 : 3;
        }
        try (PrintWriter out = new PrintWriter("pts.txt")) {
            for (double[] s : cells.values()) if (s[2] >= 3) out.printf("%.1f %.1f %d%n", s[0] / s[2], s[1] / s[2], (int)s[3]);
        }
        System.out.println(cells.size() + " cells");
    }
}
