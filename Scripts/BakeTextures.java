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
 * them: the grandstand sponsor boards (com.racingsim.track.BannerTexture), the tire smoke puff
 * atlas (com.racingsim.vehicle.TireSmoke.puffAtlas) and the worn racing slick maps
 * (com.racingsim.vehicle.TireTexture). The drawing code is copied verbatim.
 *
 *   java Scripts/BakeTextures.java SourceArt/Generated
 */
public final class BakeTextures {

    public static void main(String[] args) throws Exception {
        File dir = new File(args.length > 0 ? args[0] : ".");
        dir.mkdirs();
        ImageIO.write(banner(), "png", new File(dir, "banner.png"));
        ImageIO.write(puffAtlas(), "png", new File(dir, "smoke_puffs.png"));
        BufferedImage[] tire = tireMaps();
        ImageIO.write(tire[0], "png", new File(dir, "tire_albedo.png"));
        ImageIO.write(tire[1], "png", new File(dir, "tire_normal.png"));
        ImageIO.write(tire[2], "png", new File(dir, "tire_rough.png"));
        System.out.println("baked banner, smoke_puffs and tire_albedo/normal/rough into " + dir.getAbsolutePath());
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

    // ---------------------------------------------------------------- TireTexture.create
    // Worn racing slick for the lathe-mapped tire: u runs around the circumference, v across the
    // profile (0.00-0.30 inner sidewall, 0.38-0.62 tread, 0.70-1.00 outer sidewall). Returns the
    // albedo (sRGB), the tangent-space normal map and the roughness (gray).
    // One change from the Java colors: under Unreal's exposure the Java rubber (sRGB 0.075 sidewall,
    // 0.115 tread, about 1% reflectance) renders as flat black, so the rubber tones are lifted to
    // real tire levels (sRGB ~0.15 sidewall, ~0.22 scrubbed tread, 3-5% reflectance).
    private static final int TW = 2048, TH = 512;

    static BufferedImage[] tireMaps() {
        Random rnd = new Random(11);
        float[] height = new float[TW * TH];
        float[] rough = new float[TW * TH];
        int[] rgb = new int[TW * TH];

        float[] streak = new float[TH];
        for (int y = 0; y < TH; y++) streak[y] = rnd.nextFloat();
        float[] grainNoise = smoothNoise(TW, TH, 512, 64, rnd);
        float[] blotch = smoothNoise(TW, TH, 48, 12, rnd);

        for (int y = 0; y < TH; y++) {
            float v = 1f - (y + 0.5f) / TH;
            float treadW = band(v, 0.38f, 0.62f, 0.035f);
            float shoulder = Math.max(band(v, 0.28f, 0.40f, 0.02f), band(v, 0.60f, 0.72f, 0.02f));
            for (int x = 0; x < TW; x++) {
                int i = y * TW + x;
                float g = grainNoise[i];
                float b = blotch[i];
                float tread = 0.22f + 0.05f * (streak[y] - 0.5f) + 0.06f * (g - 0.5f) + 0.035f * (b - 0.5f);
                float side = 0.15f + 0.02f * (b - 0.5f) + 0.01f * rnd.nextFloat();
                float c = side + (tread - side) * treadW;
                float r = 0.58f + (0.86f - 0.58f) * treadW;
                float h = 0.05f * (g - 0.5f) * treadW + 0.04f * (streak[y] - 0.5f) * treadW;
                if (shoulder > 0f && rnd.nextFloat() < 0.0007f * shoulder) {
                    splat(height, rgb, rough, x, y, 1 + rnd.nextInt(3), rnd);
                }
                int cur = rgb[i];
                if (cur == 0) {
                    int n = Math.round(Math.min(1f, c) * 255f);
                    rgb[i] = (0xFF << 24) | (n << 16) | (n << 8) | (n + 1 > 255 ? 255 : n + 1);
                    rough[i] = r;
                }
                height[i] += h;
            }
        }

        BufferedImage text = new BufferedImage(TW, TH, BufferedImage.TYPE_BYTE_GRAY);
        Graphics2D g = text.createGraphics();
        g.setRenderingHint(RenderingHints.KEY_ANTIALIASING, RenderingHints.VALUE_ANTIALIAS_ON);
        g.setColor(Color.WHITE);
        drawSidewall(g, 0.735f, 0.885f, true);
        drawSidewall(g, 0.115f, 0.265f, false);
        g.dispose();
        Random wear = new Random(5);
        float[] scuff = smoothNoise(TW, TH, 256, 32, wear);
        for (int y = 0; y < TH; y++) {
            for (int x = 0; x < TW; x++) {
                int i = y * TW + x;
                float t = (text.getRaster().getSample(x, y, 0)) / 255f;
                if (t <= 0f) continue;
                height[i] += 0.9f * t;
                float paint = t * clamp01(0.55f + 0.9f * (scuff[i] - 0.45f));
                float base = (rgb[i] & 0xFF) / 255f;
                float c = base + (0.62f - base) * paint;
                int n = Math.round(c * 255f);
                rgb[i] = (0xFF << 24) | (n << 16) | (n << 8) | n;
                rough[i] = rough[i] + (0.5f - rough[i]) * paint;
            }
        }

        BufferedImage albedo = new BufferedImage(TW, TH, BufferedImage.TYPE_INT_RGB);
        albedo.setRGB(0, 0, TW, TH, rgb, 0, TW);
        BufferedImage rgh = new BufferedImage(TW, TH, BufferedImage.TYPE_INT_RGB);
        for (int i = 0; i < TW * TH; i++) {
            int r8 = Math.round(clamp01(rough[i]) * 255f);
            rgh.setRGB(i % TW, i / TW, (r8 << 16) | (r8 << 8) | r8);
        }
        return new BufferedImage[]{albedo, normalMap(height, TW, TH, 2.2f), rgh};
    }

    private static void splat(float[] height, int[] rgb, float[] rough, int cx, int cy, int r, Random rnd) {
        for (int dy = -r; dy <= r; dy++) {
            for (int dx = -r * 2; dx <= r * 2; dx++) {
                float d = (dx * dx) / (4f * r * r) + (dy * dy) / (float) (r * r);
                if (d > 1f) continue;
                int x = Math.floorMod(cx + dx, TW), y = cy + dy;
                if (y < 0 || y >= TH) continue;
                int i = y * TW + x;
                height[i] += 0.35f * (1f - d);
                int n = 28 + rnd.nextInt(8);
                rgb[i] = (0xFF << 24) | (n << 16) | (n << 8) | n;
                rough[i] = 0.92f;
            }
        }
    }

    private static void drawSidewall(Graphics2D g, float v0, float v1, boolean flip) {
        double top = (1 - v1) * TH, bottom = (1 - v0) * TH;
        double bandH = bottom - top;
        double squash = 0.42;
        Font big = new Font("Arial", Font.BOLD, 10).deriveFont((float) (bandH * 0.95));
        Font small = new Font("Arial", Font.BOLD, 10).deriveFont((float) (bandH * 0.42));
        String[][] words = {{"APEX", "big"}, {"RACING SLICK", "big"}, {"R15  275/40  ON TRACK USE ONLY", "small"}};
        double[] at = {0.04, 0.17, 0.36};
        for (int rep = 0; rep < 2; rep++) {
            for (int k = 0; k < words.length; k++) {
                Font f = "big".equals(words[k][1]) ? big : small;
                GlyphVector gv = f.createGlyphVector(g.getFontRenderContext(), words[k][0]);
                Rectangle2D b = gv.getVisualBounds();
                double cx = TW * (rep * 0.5 + at[k]) + b.getWidth() * squash / 2;
                double cy = (top + bottom) / 2;
                AffineTransform t = new AffineTransform();
                t.translate(cx, cy);
                if (flip) t.rotate(Math.PI);
                t.scale(squash, 1.0);
                t.translate(-b.getCenterX(), -b.getCenterY());
                g.fill(t.createTransformedShape(gv.getOutline()));
            }
        }
    }

    static float band(float v, float a, float b, float soft) {
        return smooth(clamp01((v - a) / soft + 0.5f)) * smooth(clamp01((b - v) / soft + 0.5f));
    }

    /** Tangent-space normal map from a height field; image row 0 is the top of the texture. */
    static BufferedImage normalMap(float[] h, int w, int hgt, float strength) {
        BufferedImage img = new BufferedImage(w, hgt, BufferedImage.TYPE_INT_RGB);
        for (int y = 0; y < hgt; y++) {
            int yu = Math.max(0, y - 1), yd = Math.min(hgt - 1, y + 1);
            for (int x = 0; x < w; x++) {
                int xl = (x - 1 + w) % w, xr = (x + 1) % w;
                float dx = (h[y * w + xr] - h[y * w + xl]) * 0.5f;
                float dv = (h[yu * w + x] - h[yd * w + x]) * 0.5f;
                float nx = -dx * strength, ny = -dv * strength, nz = 1f;
                float len = (float) Math.sqrt(nx * nx + ny * ny + nz * nz);
                int r = Math.round((nx / len * 0.5f + 0.5f) * 255f);
                int gg = Math.round((ny / len * 0.5f + 0.5f) * 255f);
                int b = Math.round((nz / len * 0.5f + 0.5f) * 255f);
                img.setRGB(x, y, (r << 16) | (gg << 8) | b);
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
