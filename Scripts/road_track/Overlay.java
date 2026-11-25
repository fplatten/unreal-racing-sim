import javax.imageio.ImageIO;
import java.awt.*;
import java.awt.image.BufferedImage;
import java.io.*;
import java.util.*;
public class Overlay {
    public static void main(String[] a) throws Exception {
        BufferedImage img = ImageIO.read(new File(a[0]));
        Graphics2D g = img.createGraphics();
        g.setRenderingHint(RenderingHints.KEY_ANTIALIASING, RenderingHints.VALUE_ANTIALIAS_ON);
        java.util.List<double[]> p = new ArrayList<>();
        for (String l : java.nio.file.Files.readAllLines(new File(a[1]).toPath())) { if (l.isBlank()) continue; String[] s = l.trim().split("\s+"); p.add(new double[]{Double.parseDouble(s[0]), Double.parseDouble(s[1])}); }
        g.setStroke(new BasicStroke(2f));
        for (int i = 0; i < p.size(); i++) {
            double[] q = p.get(i), r = p.get((i + 1) % p.size());
            float t = i / (float)p.size();
            g.setColor(Color.getHSBColor(t * 0.8f, 1f, 1f));
            g.drawLine((int)q[0], (int)q[1], (int)r[0], (int)r[1]);
            if (i % 10 == 0) { g.setColor(Color.WHITE); g.drawString("" + i, (int)q[0] + 6, (int)q[1] - 6); }
        }
        ImageIO.write(img, "png", new File(a[2]));
    }
}
