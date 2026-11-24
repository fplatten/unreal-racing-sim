import java.awt.Color;
import java.awt.Font;
import java.awt.GradientPaint;
import java.awt.Graphics2D;
import java.awt.RenderingHints;
import java.awt.font.GlyphVector;
import java.awt.geom.AffineTransform;
import java.awt.geom.Rectangle2D;
import java.awt.image.BufferedImage;
import java.io.File;
import java.util.Random;
import javax.imageio.ImageIO;

/**
 * Bakes the textures the Java game paints at start-up with AWT, so the Unreal port can import
 * them: the grandstand sponsor boards (com.racingsim.track.BannerTexture) and the tire smoke
 * puff atlas (com.racingsim.vehicle.TireSmoke.puffAtlas). The drawing code is copied verbatim.
 *
 *   java Scripts/BakeTextures.java SourceArt/Generated
 */
public final class BakeTextures {

    public static void main(String[] args) throws Exception {
        File dir = new File(args.length > 0 ? args[0] : ".");
        dir.mkdirs();
        ImageIO.write(banner(), "png", new File(dir, "banner.png"));
        ImageIO.write(puffAtlas(), "png", new File(dir, "smoke_puffs.png"));
        System.out.println("baked banner.png and smoke_puffs.png into " + dir.getAbsolutePath());
    }

    // ---------------------------------------------------------------- BannerTexture.create
    private static final String[] SPONSORS = {"APEX", "VELOCE", "TORQUE", "NOVA FUEL", "STRATOS"};
    private static final Color[][] SCHEMES = {
            {new Color(14, 40, 110), Color.WHITE},
            {new Color(240, 240, 236), new Color(200, 24, 30)},
            {new Color(20, 20, 22), new Color(250, 200, 20)},
            {new Color(196, 26, 30), Color.WHITE},
            {new Color(250, 196, 0), new Color(15, 15, 18)}};

    static BufferedImage banner() {
        int w = 4096, h = 128;
        BufferedImage img = new BufferedImage(w, h, BufferedImage.TYPE_INT_ARGB);
        Graphics2D g = img.createGraphics();
        g.setRenderingHint(RenderingHints.KEY_ANTIALIASING, RenderingHints.VALUE_ANTIALIAS_ON);
        g.setRenderingHint(RenderingHints.KEY_TEXT_ANTIALIASING, RenderingHints.VALUE_TEXT_ANTIALIAS_ON);
        int panels = SPONSORS.length;
        int pw = w / panels;
        double stretch = (h / 0.95) / (w / 40.0);
        for (int i = 0; i < panels; i++) {
            int x0 = i * pw;
            Color bg = SCHEMES[i][0], fg = SCHEMES[i][1];
            g.setPaint(new GradientPaint(0, 0, bg.brighter(), 0, h, bg.darker()));
            g.fillRect(x0, 0, pw, h);
            g.setColor(new Color(0, 0, 0, 90));
            g.fillRect(x0, 0, 3, h);
            Font f = new Font("Arial", Font.BOLD | Font.ITALIC, 10).deriveFont(h * 0.62f);
            GlyphVector gv = f.createGlyphVector(g.getFontRenderContext(), SPONSORS[i]);
            Rectangle2D b = gv.getVisualBounds();
            double sx = Math.min(1.0 / stretch, (pw * 0.8) / b.getWidth());
            AffineTransform t = new AffineTransform();
            t.translate(x0 + pw / 2.0, h / 2.0);
            t.scale(sx, 1.0);
            t.translate(-b.getCenterX(), -b.getCenterY());
            g.setColor(fg);
            g.fill(t.createTransformedShape(gv.getOutline()));
        }
        Random rnd = new Random(4);
        for (int k = 0; k < 900; k++) {
            int x = rnd.nextInt(w), y = h / 2 + rnd.nextInt(h / 2);
            g.setColor(new Color(40, 36, 30, 10 + rnd.nextInt(25)));
            g.fillRect(x, y, 1 + rnd.nextInt(3), h - y);
        }
        g.setPaint(new GradientPaint(0, h * 0.75f, new Color(30, 28, 24, 0), 0, h, new Color(30, 28, 24, 90)));
        g.fillRect(0, 0, w, h);
        g.dispose();
        return img;
    }

    // ---------------------------------------------------------------- TireSmoke.puffAtlas
    static BufferedImage puffAtlas() {
        int cell = 128, size = cell * 2;
        BufferedImage img = new BufferedImage(size, size, BufferedImage.TYPE_INT_ARGB);
        Random rnd = new Random(42);
        for (int k = 0; k < 4; k++) {
            int ox = (k % 2) * cell, oy = (k / 2) * cell;
            float[] noise = smoothNoise(cell, cell, 4, 4, rnd);
            float[] fine = smoothNoise(cell, cell, 16, 16, rnd);
            for (int y = 0; y < cell; y++) {
                for (int x = 0; x < cell; x++) {
                    float dx = (x + 0.5f) / cell * 2 - 1, dy = (y + 0.5f) / cell * 2 - 1;
                    float r = (float) Math.sqrt(dx * dx + dy * dy);
                    float n = 0.65f * noise[y * cell + x] + 0.35f * fine[y * cell + x];
                    float falloff = (float) Math.exp(-r * r * 4.0);
                    double ang = Math.atan2(dy, dx);
                    float lobes = (float) (0.5 + 0.5 * Math.sin(ang * 3 + k * 1.7) * Math.sin(ang * 5 - k));
                    float a = clamp01(falloff * (0.70f + 0.20f * n + 0.25f * lobes * r) - 0.03f) * (r < 1f ? 1f : 0f);
                    int shade = Math.min(255, Math.round(225 + 30 * n));
                    img.setRGB(ox + x, oy + y, (Math.round(a * 255) << 24) | (shade << 16) | (shade << 8) | shade);
                }
            }
        }
        return img;
    }

    static float clamp01(float v) {
        return v < 0f ? 0f : (v > 1f ? 1f : v);
    }

    private static float smooth(float t) {
        return t * t * (3 - 2 * t);
    }

    static float[] smoothNoise(int w, int h, int cellsX, int cellsY, Random rnd) {
        float[] grid = new float[cellsX * cellsY];
        for (int i = 0; i < grid.length; i++) grid[i] = rnd.nextFloat();
        float[] out = new float[w * h];
        for (int y = 0; y < h; y++) {
            float ty = (float) y * cellsY / h;
            int y0 = (int) ty;
            float fy = smooth(ty - y0);
            int y1 = (y0 + 1) % cellsY;
            y0 %= cellsY;
            for (int x = 0; x < w; x++) {
                float tx = (float) x * cellsX / w;
                int x0 = (int) tx;
                float fx = smooth(tx - x0);
                int x1 = (x0 + 1) % cellsX;
                x0 %= cellsX;
                float a = grid[y0 * cellsX + x0], b = grid[y0 * cellsX + x1];
                float c = grid[y1 * cellsX + x0], d = grid[y1 * cellsX + x1];
                out[y * w + x] = (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
            }
        }
        return out;
    }
}
