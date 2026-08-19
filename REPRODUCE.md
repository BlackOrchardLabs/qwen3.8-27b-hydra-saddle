# Reproducing the published numbers

The front-page numbers (cold 0/12, inherited 12/12) were measured on one exact model. To match them:

**The reference model**
- `Qwen3.8-27B-NVFP4-MTP-LOW.gguf` (~15.5 GB) — https://huggingface.co/esatapedico/Qwen3.8-27B-NVFP4-MTP-GGUF
- SHA-256 `ce66a629d4a3516bba27ca91de29372f086f90f72ddb92fe298de67b8bb88bbc`

**What it needs (this is the demanding part)**
- A recent NVIDIA GPU with ~22 GB free.
- A llama.cpp build with NVFP4 (GGML type 40) CUDA kernels, the `qwen35` hybrid architecture, and MTP speculative decoding. Stock llama.cpp will not load this model.

**Serve it**
```bash
llama-server -m Qwen3.8-27B-NVFP4-MTP-LOW.gguf \
  -ngl 99 --spec-type draft-mtp --spec-draft-n-max 4 \
  -c 32768 --flash-attn on --host 127.0.0.1 --port 8774
```

**Run**
Point the demos at that endpoint and the exact file. When the file's fingerprint matches the SHA-256 above, the run is comparable to the published numbers.
