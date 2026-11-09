class TfeCode < Formula
  desc "Role-tree code editor and structural renderer for AI coding agents"
  homepage "https://github.com/Artem535/TFEngineCode"
  version "0.3.0"
  license "MIT"

  on_macos do
    on_arm do
      url "https://github.com/Artem535/TFEngineCode/releases/download/v0.3.0/tfe-code-aarch64-apple-darwin.tar.gz"
      sha256 "3996eb4b73de973f790b3e9e5a5512f1dd927acf0012179901f15e42a67eedcc"
    end
  end

  on_linux do
    on_arm do
      url "https://github.com/Artem535/TFEngineCode/releases/download/v0.3.0/tfe-code-aarch64-unknown-linux-gnu.tar.gz"
      sha256 "ad762d51057d92d68a8a3dc70b51333a49c3ed7734aff86d3e8a45f01e080830"
    end
    on_intel do
      url "https://github.com/Artem535/TFEngineCode/releases/download/v0.3.0/tfe-code-x86_64-unknown-linux-gnu.tar.gz"
      sha256 "a3524d49bd752f7dc818d122b42e6d6e482c76d2b01af00cdedfc53d4b25fcdd"
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
