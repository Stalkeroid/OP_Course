# Benchmark machine

Recorded by hand for the run of 2026-11-21 (see `environment.json`).

| Item | Value |
|---|---|
| CPU | AMD Ryzen 5 7500F, 6 cores / 12 threads (SMT) |
| RAM | 32 GB DDR5 |
| GPU | NVIDIA RTX 3060 Ti 8 GB (not used: Ollama runs on the CPU in the container) |
| OS | Windows 11 |
| Docker | Docker Desktop 4.99.0, Engine 29.9.3, WSL 2 backend (kernel 6.18.58.1-microsoft-standard-WSL2) |
| Docker limits | defaults: all 12 logical CPUs visible to containers, memory up to half of host RAM |
| Dataset | generated, 20 000 documents, 2100 MiB (about 2.2 GB), 16 656 unique index terms |
| Model | `llama3.2:1b` via Ollama, CPU inference |
| Load generator | the `bench` compose service on the same machine, sharing the CPU with the server |
