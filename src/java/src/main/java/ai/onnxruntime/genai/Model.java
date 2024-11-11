/*
 * Copyright (c) Microsoft Corporation. All rights reserved. Licensed under the MIT License.
 */
package ai.onnxruntime.genai;

public final class Model implements AutoCloseable {
  private long nativeHandle;

  public Model(String modelPath) throws GenAIException {
    nativeHandle = createModel(modelPath);
  }

  public Model(Config config) throws GenAIException {
    nativeHandle = createModelFromConfig(config.nativeHandle());
  }

  /**
   * Creates a Tokenizer instance for this model. The model contains the configuration information
   * that determines the tokenizer to use.
   *
   * @return The Tokenizer instance.
   * @throws GenAIException If the call to the GenAI native API fails.
   */
  public Tokenizer createTokenizer() throws GenAIException {
    if (nativeHandle == 0) {
      throw new IllegalStateException("Instance has been freed and is invalid");
    }

    return new Tokenizer(this);
  }

  // NOTE: Having model.createGeneratorParams is still under discussion.
  // model.createTokenizer is consistent with the python setup at least and agreed upon.

  /**
   * Creates a GeneratorParams instance for executing the model. NOTE: GeneratorParams internally
   * uses the Model, so the Model instance must remain valid
   *
   * @return The GeneratorParams instance.
   * @throws GenAIException If the call to the GenAI native API fails.
   */
  public GeneratorParams createGeneratorParams() throws GenAIException {
    if (nativeHandle == 0) {
      throw new IllegalStateException("Instance has been freed and is invalid");
    }

    return new GeneratorParams(this);
  }

  /**
   * Run the model to generate output sequences. Generation is limited to the "max_length" value
   * (default:300) in the generator parameters. Use a Tokenizer to decode the generated sequences.
   *
   * @param generatorParams The generator parameters.
   * @return The generated sequences.
   * @throws GenAIException If the call to the GenAI native API fails.
   */
  public Sequences generate(GeneratorParams generatorParams) throws GenAIException {
    if (generatorParams.nativeHandle() == 0) {
      throw new IllegalArgumentException("generatorParams has been freed and is invalid");
    }

    if (nativeHandle == 0) {
      throw new IllegalStateException("Instance has been freed and is invalid");
    }

    long sequencesHandle = generate(nativeHandle, generatorParams.nativeHandle());
    return new Sequences(sequencesHandle);
  }

  @Override
  public void close() {
    if (nativeHandle != 0) {
      destroyModel(nativeHandle);
      nativeHandle = 0;
    }
  }

  long nativeHandle() {
    return nativeHandle;
  }

  static {
    try {
      GenAI.init();
    } catch (Exception e) {
      throw new RuntimeException("Failed to load onnxruntime-genai native libraries", e);
    }
  }

  private native long createModel(String modelPath) throws GenAIException;

  private native long createModelFromConfig(long configHandle) throws GenAIException;

  private native void destroyModel(long modelHandle);

  private native long generate(long modelHandle, long generatorParamsHandle) throws GenAIException;
}
