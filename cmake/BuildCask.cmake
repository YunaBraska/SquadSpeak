# Generate the tap entry from the exact archives already verified by the matrix.
if(NOT DEFINED SQUADSPEAK_VERSION OR NOT IS_DIRECTORY "${ARCHIVES_DIR}")
    message(FATAL_ERROR "SQUADSPEAK_VERSION and ARCHIVES_DIR are required")
endif()
include("${CMAKE_CURRENT_LIST_DIR}/Version.cmake")
foreach(arch arm64 x86_64)
    set(archive "${ARCHIVES_DIR}/squadspeak-macos-${arch}.zip")
    if(NOT EXISTS "${archive}" OR IS_DIRECTORY "${archive}")
        message(FATAL_ERROR "Missing tested archive: ${archive}")
    endif()
    file(SIZE "${archive}" archive_size)
    if(archive_size EQUAL 0)
        message(FATAL_ERROR "Empty tested archive: ${archive}")
    endif()
    file(SHA256 "${archive}" sha_${arch})
endforeach()
file(WRITE "${ARCHIVES_DIR}/squadspeak.rb" "cask \"squadspeak\" do
  # yuna-release: YunaBraska/SquadSpeak
  # yuna-release-asset: squadspeak-macos-arm64.zip
  # yuna-release-asset: squadspeak-macos-x86_64.zip
  arch arm: \"arm64\", intel: \"x86_64\"

  version \"${SQUADSPEAK_VERSION}\"
  sha256 arm:   \"${sha_arm64}\",
         intel: \"${sha_x86_64}\"

  url \"https://github.com/YunaBraska/SquadSpeak/releases/download/#{version}/squadspeak-macos-#{arch}.zip\"
  name \"SquadSpeak\"
  desc \"Voice and chat for local networks and private servers\"
  homepage \"https://github.com/YunaBraska/SquadSpeak\"

  depends_on macos: :ventura

  app \"squadspeak.app\"
  binary \"#{appdir}/squadspeak.app/Contents/MacOS/squadspeak\"
end
")
