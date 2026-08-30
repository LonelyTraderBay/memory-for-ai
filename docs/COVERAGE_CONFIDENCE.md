# Coverage and confidence reporting

index_status and check_index_coverage now expose a coverage_summary alongside
the detailed per-file gap lists.

The summary contains:

- indexed_file_hashes: authoritative files represented by the current
  file_hashes manifest.
- parse_partial_files: files hashed and indexed, but with parser error ranges.
- skipped_files: files that could not be indexed at all.
- excluded_files and excluded_directories: paths deliberately excluded by
  ignore rules.
- coverage_ratio: the known file-hash ratio, or null when the denominator
  cannot be proven.
- confidence_score and confidence_level: how trustworthy the summary is,
  based on metadata availability, generation matching, complete hash records,
  complete coverage recording, known directory boundaries, and known gaps.
- confidence_reasons: machine-readable reasons supporting the score.

The ratio is intentionally not a parser-accuracy score:

    indexed_file_hashes / (indexed_file_hashes + skipped_files + excluded_files)

parse_partial_files remain in the numerator because they have a valid file-hash
record; they are reported separately so callers can decide whether to read the
flagged ranges directly. The ratio is null when an excluded directory hides an
unknown number of files, metadata is truncated, the generation does not match,
or hash records are incomplete.

Confidence is a trust signal, not a guarantee that the graph is complete.
high means the accounting inputs are internally consistent; it does not replace
source inspection for security-critical or exhaustive conclusions.
