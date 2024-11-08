/*
 * Copyright (c) Microsoft Corporation. All rights reserved.
 * Licensed under the MIT License.
 */
package ai.onnxruntime.genai;

import java.util.logging.Logger;
import org.junit.jupiter.api.Test;

// NOTE: Typical usage is covered in GenerationTest.java so we are just filling test gaps here.
public class MultiModalProcessorTest {
  private static final Logger logger = Logger.getLogger(MultiModalProcessorTest.class.getName());

  @Test
  public void testBatchEncodeDecode() throws GenAIException {
    try (Model model = new Model(TestUtils.testVisioniModelPath());
        MultiModalProcessor multiModalProcessor = new MultiModalProcessor(model)) {
      TokenizerStream stream = multiModalProcessor.createStream();
      GeneratorParams generatorParams = model.createGeneratorParams();
      String inputs =
          new String(
              "<|user|>\n<|image_1|>\n Can you convert the table to markdown format?\n<|end|>\n<|assistant|>\n");
      Images image = new Images(TestUtils.getFilePathFromResource("/images/sheet.png"));
      NamedTensors processed = multiModalProcessor.processImages(inputs, image);

      processed.close();
      image.close();
      generatorParams.close();
      stream.close();
    }
  }
}
