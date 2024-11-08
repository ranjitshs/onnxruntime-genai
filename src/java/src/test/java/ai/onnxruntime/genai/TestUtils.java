/*
 * Copyright (c) Microsoft Corporation. All rights reserved.
 * Licensed under the MIT License.
 */
package ai.onnxruntime.genai;

import java.io.File;
import java.net.URL;
import java.util.logging.Logger;

public class TestUtils {
  private static final Logger logger = Logger.getLogger(TestUtils.class.getName());

  public static final String testAdapterTestModelPath() {
    // get the resources directory from one of the classes
    URL url = TestUtils.class.getResource("/adapters");
    if (url == null) {
      logger.warning("Model not found at /adapters");
      return null;
    }

    File f = new File(url.getFile());
    return f.getPath();
  }

  public static final String testAdapterTestAdaptersPath() {
    // get the resources directory from one of the classes
    URL url = TestUtils.class.getResource("/adapters/adapters.onnx_adapter");
    if (url == null) {
      logger.warning("Model not found at /adapters/adapters.onnx_adapter");
      return null;
    }

    File f = new File(url.getFile());
    return f.getPath();
  }

  public static final String testModelPath() {
    return getFilePathFromResource("/hf-internal-testing/tiny-random-gpt2-fp32");
  }

  public static final String testVisioniModelPath() {
    return getFilePathFromResource("/vision-preprocessing");
  }

  public static final String getRepoRoot() {
    String classDirFileUrl = SimpleGenAI.class.getResource("").getFile();
    String repoRoot = classDirFileUrl.substring(0, classDirFileUrl.lastIndexOf("src/java/build"));
    return repoRoot;
  }

  public static final boolean setLocalNativeLibraryPath() {
    // set to <build output dir>/src/java/native-jni/ai/onnxruntime/genai/native/win-x64,
    // adjusting for your build output location and platform as needed
    String nativeJniBuildOutput =
        "build/Windows/Debug/src/java/native-jni/ai/onnxruntime/genai/native/win-x64";
    File fullPath = new File(getRepoRoot() + nativeJniBuildOutput);
    if (!fullPath.exists()) {
      logger.warning("Local native-jni build output not found at: " + fullPath.getPath());
      return false;
    }

    System.setProperty("onnxruntime-genai.native.path", fullPath.getPath());
    return true;
  }

  static final String getFilePathFromResource(String path) {
    // get the resources directory from one of the classes
    URL url = TestUtils.class.getResource(path);
    if (url == null) {
      logger.warning("Model not found at " + path);
      return null;
    }

    File f = new File(url.getFile());
    return f.getPath();
  }
}
