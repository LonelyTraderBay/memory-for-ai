class MemoryForAi < Formula
  desc "Fast code intelligence engine for AI coding agents"
  homepage "https://github.com/LonelyTraderBay/memory-for-ai"
  version "0.11.0"
  license "MIT"

  on_macos do
    on_arm do
      url "https://github.com/LonelyTraderBay/memory-for-ai/releases/download/v#{version}/memory-for-ai-darwin-arm64.tar.gz"
      sha256 "9f84f06588d3df00f2fc9d3bad254e79a9159c42fa9f308e561bf8362b50b8f6"
    end
    on_intel do
      url "https://github.com/LonelyTraderBay/memory-for-ai/releases/download/v#{version}/memory-for-ai-darwin-amd64.tar.gz"
      sha256 "26b265bb00c1cfae50ddccac8681baed20e96553693fa6146525ae043be82426"
    end
  end

  on_linux do
    on_arm do
      url "https://github.com/LonelyTraderBay/memory-for-ai/releases/download/v#{version}/memory-for-ai-linux-arm64.tar.gz"
      sha256 "54e405819cbd22b0810374ab6bd65c97a721ab7dde4a630166bc0ed90f327803"
    end
    on_intel do
      url "https://github.com/LonelyTraderBay/memory-for-ai/releases/download/v#{version}/memory-for-ai-linux-amd64.tar.gz"
      sha256 "1b5234d0d3cb086cfc56dd0faf374abb9b388ad3222b5302a74f070aa94c5e87"
    end
  end

  def install
    bin.install "memory-for-ai"
    # Third-party attribution bundle (present in archives since v0.8.1)
    doc.install "THIRD_PARTY_NOTICES.md" if File.exist?("THIRD_PARTY_NOTICES.md")
  end

  def caveats
    <<~EOS
      Run the following to configure your coding agents:
        memory-for-ai install

      To tap this formula directly:
        brew tap lonelytraderbay/memory-for-ai https://github.com/LonelyTraderBay/memory-for-ai
        brew install memory-for-ai
    EOS
  end

  test do
    assert_match "memory-for-ai", shell_output("#{bin}/memory-for-ai --version")
  end
end
