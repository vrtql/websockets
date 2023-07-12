;; Source file: http://www.dd.chalmers.se/~bojohan/emacs/lisp/my-htmlize.el
;; Output:      http://www.dd.chalmers.se/~bojohan/emacs/out.html

;; Make sure the the htmlize library is in load-path.
;; You might want to load ~/.emacs

;; USAGE:
;;     emacs -batch -l my-htmlize.el INFILE > OUTFILE

(load "htmlize.el")

(find-file (pop command-line-args-left))

(global-font-lock-mode)
(setq font-lock-maximum-decoration t)
(font-lock-fontify-buffer) 

(with-current-buffer 
  (htmlize-buffer)
  (princ (buffer-string))
)
