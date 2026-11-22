# One-shot dataset initializer: generates the synthetic corpus into the dataset
# volume (skipped if a complete dataset with the same parameters already exists).
FROM python:3.12-slim
WORKDIR /app
COPY scripts/generate_dataset.py scripts/dataset_vocabulary.py ./
ENV PYTHONUNBUFFERED=1
ENTRYPOINT ["python", "/app/generate_dataset.py"]
