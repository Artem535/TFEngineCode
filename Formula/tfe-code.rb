class TfeCode < Formula
  desc "Role-tree code editor and structural renderer for AI coding agents"
  homepage "https://github.com/Artem535/TFEngineCode"
  version "0.2.1"
  license "MIT"

  on_macos do
    on_arm do
      url "https://github.com/Artem535/TFEngineCode/releases/download/v0.2.1/tfe-code-aarch64-apple-darwin.tar.gz"
      sha256 "0000000000000000000000000000000000000000000000000000000000000000"
    end
  end

  on_linux do
    on_arm do
      url "https://github.com/Artem535/TFEngineCode/releases/download/v0.2.1/tfe-code-aarch64-unknown-linux-gnu.tar.gz"
      sha256 "0000000000000000000000000000000000000000000000000000000000000000"
    end
    on_intel do
      url "https://github.com/Artem535/TFEngineCode/releases/download/v0.2.1/tfe-code-x86_64-unknown-linux-gnu.tar.gz"
      sha256 "f9c0811e8ea7d687c5832625bd5b41e650a31ff6ef6f99f62f51a05d155ef054"
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
    assert_match "Usage: tfe-code", shell_output("#{bin}/tfe-code help")
  end
end
