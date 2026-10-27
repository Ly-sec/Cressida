;; ping @midnight-stampede for dependency changes or
;; build failures

(define-module (cressida)
  #:use-module (guix packages)
  #:use-module (guix gexp)
  #:use-module (guix git-download)
  #:use-module (guix build-system meson)
  #:use-module (guix build-system cmake)
  #:use-module ((guix licenses) #:prefix license:)
  #:use-module (guix utils) ;current-source-directory
  #:use-module (gnu packages)
  #:use-module (gnu packages aidc) ;qrencode
  #:use-module (gnu packages pkg-config) ;pkg-config
  #:use-module (gnu packages freedesktop) ;wayland, wayland-protocols, xdg-utils
  #:use-module (gnu packages curl) ;curl
  #:use-module (gnu packages fontutils) ;fontconfig, freetype
  #:use-module (gnu packages fonts) ;font-google-noto-emoji
  #:use-module (gnu packages gl) ;libglvnd, mesa
  #:use-module (gnu packages glib) ;glib, dbus
  #:use-module (gnu packages gnome) ;libsecret, librsvg
  #:use-module (gnu packages image) ;libjxl, libwebp, wuffs
  #:use-module (gnu packages python) ;python
  #:use-module (gnu packages gtk) ;cairo, harfbuzz, pango
  #:use-module (gnu packages video) ;mpv
  #:use-module (gnu packages xdisorg) ;libxkbcommon
  #:use-module (gnu packages markup) ;md4c
  #:use-module (gnu packages animation) ;rlottie
  #:use-module (gnu packages stb) ;stb-image-resize2
  #:use-module (gnu packages compression) ;zstd:lib
  #:use-module (gnu packages crypto) ;libsodium
  #:use-module (gnu packages pulseaudio) ;libpulse
  #:use-module (gnu packages tls) ;openssl
  #:use-module (gnu packages xiph) ;opus
  #:use-module (gnu packages cpp)) ;nlohmann-json, tomlplusplus

(define-public mlspp-for-dave
  (let ((commit "1cc50a124a3bc4e143a787ec934280dc70c1034d")
        (revision "0"))
    (package
      (name "mlspp-for-dave")
      (version (git-version "0.1.0" revision commit))
      (source
       (origin
         (method git-fetch)
         (uri (git-reference
               (url "https://github.com/cisco/mlspp")
               (commit commit)))
         (file-name (git-file-name name version))
         (sha256
          (base32 "1w3j520vadfy72ql1x5080v600zcfyd0qskh8w4wqjfzi74vcd12"))))
      (build-system cmake-build-system)
      (arguments
       (list
        #:tests? #f
        #:configure-flags
        #~(list "-DMLS_CXX_NAMESPACE=mlspp"
                "-DDISABLE_GREASE=ON"
                ;; Needed so the static libs can be linked into
                ;; the shared libdave.so.
                "-DCMAKE_POSITION_INDEPENDENT_CODE=ON")
        #:phases
        #~(modify-phases %standard-phases
            (add-after 'install 'install-pkg-config
              (lambda* (#:key outputs #:allow-other-keys)
                (let* ((out (assoc-ref outputs "out"))
                       (pc-dir (string-append out "/lib/pkgconfig")))
                  (mkdir-p pc-dir)
                  (call-with-output-file (string-append pc-dir "/mlspp.pc")
                    (lambda (port)
                      (format port "prefix=~a
libdir=${prefix}/lib
includedir=${prefix}/include/mlspp

Name: mlspp
Description: MLS group protocol
Version: 0.1
Libs: -L${libdir} -lmlspp -lhpke -ltls_syntax -lbytes
Cflags: -I${includedir}
"
                              out)))))))))
      (native-inputs (list pkg-config))
      (inputs (list nlohmann-json openssl))
      (home-page "https://github.com/cisco/mlspp")
      (synopsis "Implementation of Messaging Layer Security")
      (description "Implementation of Messaging Layer Security, this package is for use
specifically with libdave.")
      (license license:bsd-2))))

(define-public libdave
  (package
    (name "libdave")
    (version "1.2.2")
    (source
     (origin
       (method git-fetch)
       (uri (git-reference
             (url "https://github.com/discord/libdave")
             (commit (string-append "v" version "/cpp"))))
       (file-name (git-file-name name version))
       (sha256
        (base32 "0rna0mp45w1qls02p8xrwmh4yg9jdjl10w45zim563jlpgbrykwi"))))
    (build-system cmake-build-system)
    (arguments
     (list
      #:tests? #f
      #:configure-flags
      #~(list "-DBUILD_SHARED_LIBS=ON")
      #:phases
      #~(modify-phases %standard-phases
          (add-after 'unpack 'cpp-dir
            (lambda _ (chdir "cpp")))
          (add-after 'install 'install-pkg-config
            (lambda* (#:key outputs #:allow-other-keys)
              (let* ((out (assoc-ref outputs "out"))
                     (pc-dir (string-append out "/lib/pkgconfig")))
                (mkdir-p pc-dir)
                (call-with-output-file (string-append pc-dir "/libdave.pc")
                  (lambda (port)
                    (format port "prefix=~a
libdir=${prefix}/lib
includedir=${prefix}/include

Name: libdave
Description: Audio frame encryption and MLS sessions
Version: ~a
Libs: -L${libdir} -ldave
Cflags: -I${includedir}
"
                            out #$(package-version this-package))))))))))
    (native-inputs (list pkg-config))
    (inputs (list mlspp-for-dave
                  openssl
                  nlohmann-json))
    (home-page "https://daveprotocol.com/")
    (synopsis "Libraries supporting Discord's DAVE protocol")
    (description "Libraries supporting Discord's Audio & Video End-to-End Encryption
(DAVE) protocol")
    (license license:expat)))

(define source-checkout
  (local-file "." "cressida-checkout"
              #:recursive? #t
              #:select?
              (or (git-predicate (current-source-directory))
                  (const #t))))

(define-public cressida-git
  (package
    (name "cressida-git")
    (version "latest")
    (source source-checkout)
    (build-system meson-build-system)
    (arguments
     (list
      ;; #:configure-flags
      ;; #~(list (string-append "-Demoji_font="
      ;;                        #$(this-package-input "font-google-noto-emoji")
      ;;                        "/share/fonts/truetype/NotoColorEmoji.ttf"))
      #:build-type "release"
      #:phases
      #~(modify-phases %standard-phases
          (add-after 'unpack 'use-system-wuffs
            (lambda _
              (delete-file-recursively "third_party")
              ;; so meson wouldn't complain
              (substitute* "meson.build"
                (("'src', 'third_party/wuffs'")
                 "'src'")
                ((", 'third_party/wuffs/LICENSE-APACHE', 'third_party/wuffs/LICENSE-MIT'")
                 ""))
              ;; so tests wouldn't complain
              (substitute* "tests/installation_test.py"
                (("\\(\"LICENSE\", \"LICENSE-APACHE\", \"LICENSE-MIT\"\\)")
                 "(\"LICENSE\",)"))))

          (add-before 'configure 'fix-native-controls-test
            (lambda _
              (substitute* "meson.build"
                (("test\\('native_controls', control_test\\)")
                 (string-append
                  "test('native_controls', control_test,\n"
                  "  env: ['CRESSIDA_ASSETS_DIR=' + meson.project_source_root() / 'assets']\n"
                  ")")))))

          (add-after 'unpack 'fix-stb-header-path
            (lambda _
              (substitute* "meson.build"
                (("stb/stb_image_resize2.h")
                 "stb_image_resize2.h"))
              (substitute* (find-files "." "\\.(cpp|c|h|hpp)$")
                (("#include <stb/stb_image_resize2.h>")
                 "#include <stb_image_resize2.h>"))))

          (add-before 'check 'set-test-tmpdir
            (lambda _
              ;; Prevent test failure due to too long a socket path
              (setenv "TMPDIR" "/tmp")))

          (add-before 'check 'set-up-test-environment
            (lambda* (#:key inputs #:allow-other-keys)
              (setenv "HOME" (getcwd))
              (setenv "XDG_RUNTIME_DIR" (getcwd))
              (setenv "XDG_DATA_DIRS"
                      (string-append
                       (assoc-ref inputs "shared-mime-info") "/share")))))))
    (native-inputs (list pkg-config
                         python ; for tests
                         wayland
                         xdg-utils ; for tests
                         dbus ; for tests
                         wayland-protocols))
    (inputs (list cairo
                  curl
                  ffmpeg
                  fontconfig
                  freetype
                  glib
                  harfbuzz
                  libjxl
                  libsecret
                  wuffs
                  stb-image-resize2
                  stb-image-write
                  (librsvg-for-system)
                  libwebp
                  libxkbcommon
                  md4c
                  mesa
                  mpv
                  nlohmann-json
                  openssl
                  (list zstd "lib")
                  pango
                  qrencode
                  rlottie
                  tomlplusplus
                  libsodium
                  pulseaudio
                  libdave
                  opus
                  mlspp-for-dave))
    (home-page "https://github.com/Ly-sec/Cressida")
    (synopsis "Native C++ Discord desktop client")
    (description "A native Discord desktop client for Linux and Wayland.")
    (license license:expat)))

cressida-git
