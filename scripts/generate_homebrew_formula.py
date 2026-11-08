#!/usr/bin/env python3
"""Generate Homebrew formula for TFEngine Code."""

import argparse
from pathlib import Path
import re

HEX64 = re.compile(r"^[0-9a-fA-F]{64}$")


def validate_sha(name: str, value: str) -> str:
    cleaned = value.strip().lower()
    if not HEX64.fullmatch(cleaned):
        raise ValueError(f"Invalid SHA-256 for {name}: {value!r}")
    return cleaned


def render_formula(
    version: str,
    sha_linux_x86_64: str,
    sha_linux_aarch64: str,
    sha_macos_aarch64: str,
    repo: str = "Artem535/TFEngineCode",
) -> str:
    clean_version = version.lstrip("v")
    if not clean_version:
        raise ValueError("Version must not be empty")
    tag = f"v{clean_version}"
    sha_x64 = validate_sha("linux_x86_64", sha_linux_x86_64)
    sha_arm64_linux = validate_sha("linux_aarch64", sha_linux_aarch64)
    sha_arm64_macos = validate_sha("macos_aarch64", sha_macos_aarch64)

    return f"""class TfeCode < Formula
  desc "Role-tree code editor and structural renderer for AI coding agents"
  homepage "https://github.com/{repo}"
  version "{clean_version}"
  license "MIT"

  on_macos do
    on_arm do
      url "https://github.com/{repo}/releases/download/{tag}/tfe-code-aarch64-apple-darwin.tar.gz"
      sha256 "{sha_arm64_macos}"
    end
  end

  on_linux do
    on_arm do
      url "https://github.com/{repo}/releases/download/{tag}/tfe-code-aarch64-unknown-linux-gnu.tar.gz"
      sha256 "{sha_arm64_linux}"
    end
    on_intel do
      url "https://github.com/{repo}/releases/download/{tag}/tfe-code-x86_64-unknown-linux-gnu.tar.gz"
      sha256 "{sha_x64}"
    end
  end

  def install
    libexec.install "tfe-code"
    Dir["libobjectbox*"].each do |lib|
      libexec.install lib
    end
    bin.install_symlink libexec/"tfe-code"
  end

  test do
    assert_match "Usage: tfe-code", shell_output("#{{bin}}/tfe-code help")
  end
end
"""


def main() -> None:
    parser = argparse.ArgumentParser(description="Generate Homebrew formula for tfe-code")
    parser.add_argument("--version", required=True, help="Release version or tag (e.g. v0.2.1)")
    parser.add_argument("--sha-linux-x86-64", required=True, help="SHA-256 for x86_64-unknown-linux-gnu")
    parser.add_argument("--sha-linux-aarch64", required=True, help="SHA-256 for aarch64-unknown-linux-gnu")
    parser.add_argument("--sha-macos-aarch64", required=True, help="SHA-256 for aarch64-apple-darwin")
    parser.add_argument("--repo", default="Artem535/TFEngineCode", help="GitHub repository slug")
    parser.add_argument("--output", default="Formula/tfe-code.rb", help="Output formula path")
    args = parser.parse_args()

    content = render_formula(
        version=args.version,
        sha_linux_x86_64=args.sha_linux_x86_64,
        sha_linux_aarch64=args.sha_linux_aarch64,
        sha_macos_aarch64=args.sha_macos_aarch64,
        repo=args.repo,
    )
    out_path = Path(args.output)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(content, encoding="utf-8")


if __name__ == "__main__":
    main()
