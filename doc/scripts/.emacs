(if (fboundp 'scroll-bar-mode) (scroll-bar-mode -1))
(if (fboundp 'tool-bar-mode) (tool-bar-mode -1))
(if (fboundp 'menu-bar-mode) (menu-bar-mode -1))

(setq-default fill-column 80)
(setq-default tab-width 4)
(setq-default indent-tabs-mode nil)

(setq auto-mode-alist
     (cons '("\\.\\(rb\\|rsp\\|rhtml\\)\\'" . ruby-mode) auto-mode-alist))
(setq auto-mode-alist
     (cons '("\\.h$" . c++-mode) auto-mode-alist))
(setq auto-mode-alist
     (cons '("\\.rb$" . ruby-mode) auto-mode-alist))
(setq auto-mode-alist
     (cons '("\\.cc" . c++-mode) auto-mode-alist))
(setq auto-mode-alist
     (cons '("\\.cpp$" . c++-mode) auto-mode-alist))

(autoload 'yaml-mode "yaml-mode" "YAML editing mode." t)
(setq auto-mode-alist
     (cons '("\\.yml$" . yaml-mode) auto-mode-alist))

(setq auto-mode-alist
     (cons '("\\.js$" . javascript-mode) auto-mode-alist))

(setq ruby-indent-level 2)

(setq load-path (cons "scripts" load-path))
(load-library "javascript.el")
(autoload 'javascript-mode "javascript" "Javascript editing mode." t)

(setq auto-mode-alist
     (cons '("\\.js$" . javascript-mode) auto-mode-alist))

(custom-set-variables
  ;; custom-set-variables was added by Custom -- don't edit or cut/paste it!
  ;; Your init file should contain only one such instance.
 '(asm-comment-char 35)
 '(c-basic-offset 4)
'(c++-font-lock-extra-types
     (quote ("i8" "u8" "i16" "u16" 
             "i32" "u32" "i64" "u64" 
             "f32" "f64" "f128" "addr")))
 '(c-comment-only-line-offset 0)
 '(c-continued-statement-offset 4)
 '(c-default-style "stroustrup")
 '(c-label-minimum-indentation 4)
 '(c-strict-syntax-p nil)
 '(c-style-variables-are-local-p t)
 '(cperl-continued-statement-offset 0)
 '(cperl-indent-level 4)
 '(cperl-label-offset -1)
 '(cperl-min-label-indent 4)
 '(cperl-tab-always-indent t)
 '(latex-symbol-directory "/tmp/")
 '(line-number-mode t)
 '(load-home-init-file t t)
 '(next-line-add-newlines nil)
 '(query-user-mail-address nil)
 '(tab-width 4))

(custom-set-faces
  ;; custom-set-faces was added by Custom -- don't edit or cut/paste it!
  ;; Your init file should contain only one such instance.
 '(default ((t (:foreground "green" :size "18pt"))))
 '(custom-button-face ((t nil)))
 '(custom-documentation-face ((t nil)))
 '(custom-set-face (((:foreground "blue" :background "green"))))
 '(custom-variable-button-face ((t (:underline t))))
 '(custom-variable-tag-face ((t (:underline t :foreground "purple"))))
 '(dired-face-directory ((t (:foreground "blue"))))
 '(font-lock-comment-face ((t (:foreground "white"))))
 '(font-lock-builtin-face ((t (:foreground "goldenrod" :bold t))))
 '(font-lock-variable-name-face ((t (:foreground "sea green" :bold t))))
 '(font-lock-type-face ((t (:foreground "olive drab" :weight bold))))
 '(font-lock-function-name-face ((t (:foreground "yellow" :weight bold))))
 '(font-lock-keyword-face ((t (:foreground "cornflowerblue" :weight bold))))
 '(font-lock-preprocessor-face ((t (:foreground "yellow"))))
 '(font-lock-string-face ((t (:foreground "snow3"))))
 '(font-lock-constant-face ((t (:foreground "DarkOrange3" :weight bold))))
 '(font-lock-warning-face ((t (:foreground "red"))))
 '(isearch ((t (:foreground "black" :background "yellow"))))
 '(nxml-attribute-local-name-face ((t (:inherit nxml-name-face :foreground "yellow" :weight bold))))
 '(nxml-attribute-prefix-face ((t (:inherit nxml-name-face :foreground "blue"))))
 '(nxml-attribute-value-face ((t (:inherit nxml-delimited-data-face :foreground "white"))))
 '(nxml-cdata-section-content-face ((t (:inherit nxml-text-face :foreground "pink"))))
 '(nxml-comment-content-face ((t (:foreground "white" :slant italic))))
 '(nxml-comment-delimiter-face ((t (:inherit nxml-delimiter-face :foreground "white"))))
 '(nxml-delimited-data-face ((nil (:foreground "yellow"))))
 '(nxml-delimiter-face ((t (:foreground "white" :weight bold))))
 '(nxml-element-colon-face ((t (:inherit nxml-name-face :foreground "yellow" :weight bold))))
 '(nxml-element-local-name-face ((t (:inherit nxml-name-face :foreground "blue"))))
 '(nxml-entity-ref-delimiter-face ((t (:inherit nxml-ref-face :foreground "orange" :weight bold))))
 '(nxml-entity-ref-name-face ((t (:inherit nxml-ref-face :foreground "orange" :weight bold))))
 '(nxml-hash-face ((t (:inherit nxml-name-face :foreground "blue"))))
 '(nxml-name-face ((nil (:foreground "cornflowerblue" :weight bold))))
 '(pointer ((t (:foreground "cornflowerblue")))))

(setq minibuffer-max-depth nil)
(global-font-lock-mode)
(setq font-lock-maximum-decoration t)
