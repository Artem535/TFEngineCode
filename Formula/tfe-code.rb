class TfeCode < Formula
  desc "Role-tree code editor and structural renderer for AI coding agents"
  homepage "https://github.com/Artem535/TFEngineCode"
  version "0.4.1"
  license "MIT"

  on_macos do
    on_arm do
      url "https://github.com/Artem535/TFEngineCode/releases/download/v0.4.1/tfe-code-aarch64-apple-darwin.tar.gz"
      sha256 "95a33714c06d0f68ca16bd5c4f5035c285494985476970c9dddd794c46ba9562"
    end
  end

  on_linux do
    on_arm do
      url "https://github.com/Artem535/TFEngineCode/releases/download/v0.4.1/tfe-code-aarch64-unknown-linux-gnu.tar.gz"
      sha256 "400db555ee6772790fedd2aeb103a3fbd80da4c3b591b2ab06dcd02a2b92d717"
    end
    on_intel do
      url "https://github.com/Artem535/TFEngineCode/releases/download/v0.4.1/tfe-code-x86_64-unknown-linux-gnu.tar.gz"
      sha256 "e59be128f9fd5409e7d71c8be4234cea983b195188b2db8ed7ce3176049ef5f5"
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
