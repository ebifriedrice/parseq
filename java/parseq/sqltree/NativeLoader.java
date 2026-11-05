package parseq.sqltree;

import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.util.Locale;

/**
 * Locates and loads libsqltree for the JNI binding.
 *
 * Search order: -Dparseq.lib=<absolute path>, then java.library.path
 * (plain loadLibrary), then the copy embedded in the parseq jar for
 * the running os/arch, extracted to a temp file. The embedded lookup
 * is side-effect free and public so non-JNI bindings (Panama) can
 * resolve the same artifact themselves.
 */
public final class NativeLoader {
  private static String embedded;

  private NativeLoader() {}

  /** Load the library for JNI use; throws UnsatisfiedLinkError when nothing is found. */
  public static void load() {
    String prop = System.getProperty("parseq.lib");
    if (prop != null && !prop.isEmpty()) {
      System.load(prop);
      return;
    }
    try {
      System.loadLibrary("sqltree");
      return;
    } catch (UnsatisfiedLinkError e) {
      // not on java.library.path; fall through to the embedded copy
    }
    String path = embeddedLibraryPath();
    if (path == null) {
      throw new UnsatisfiedLinkError(
          "no libsqltree for " + os() + "/" + arch()
          + ": set -Dparseq.lib or -Djava.library.path, or use a"
          + " parseq jar that carries the native library");
    }
    System.load(path);
  }

  /**
   * Extract this platform's embedded copy (resource
   * parseq/native/<os>/<arch>/<lib>) to a temp file and return its
   * absolute path; null when the classpath carries none. Memoized per
   * JVM.
   */
  public static synchronized String embeddedLibraryPath() {
    if (embedded != null) {
      return embedded;
    }
    String name = System.mapLibraryName("sqltree");
    String res = "/parseq/native/" + os() + "/" + arch() + "/" + name;
    InputStream in = NativeLoader.class.getResourceAsStream(res);
    if (in == null) {
      return null;
    }
    try (InputStream src = in) {
      Path tmp = Files.createTempFile("libsqltree-",
                                      name.substring(name.lastIndexOf('.')));
      Files.copy(src, tmp, StandardCopyOption.REPLACE_EXISTING);
      tmp.toFile().deleteOnExit();
      embedded = tmp.toAbsolutePath().toString();
      return embedded;
    } catch (IOException e) {
      return null;
    }
  }

  private static String os() {
    String name = System.getProperty("os.name", "").toLowerCase(Locale.ROOT);
    if (name.contains("win")) {
      return "windows";
    }
    if (name.contains("mac") || name.contains("darwin")) {
      return "macos";
    }
    return "linux";
  }

  private static String arch() {
    String a = System.getProperty("os.arch", "").toLowerCase(Locale.ROOT);
    if (a.equals("x86_64") || a.equals("amd64")) {
      return "amd64";
    }
    if (a.equals("aarch64") || a.equals("arm64")) {
      return "arm64";
    }
    return a;
  }
}
