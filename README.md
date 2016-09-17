# apple.github.io/swift-corelibs-libdispatch

This branch contains the [apple.github.io/swift-corelibs-libdispatch](https://apple.github.io/swift-corelibs-libdispatch/) web site.

## Testing changes locally

You should test any changes to the web site locally before pushing.

In the Terminal, `cd` to the root directory of your clone.

### Install Ruby, bundler, and the GitHub Pages gems

Make sure you have Ruby 2.0.0 or greater:

    ruby --version

Install the bundler gem:

    gem install bundler

Install the bundle of gems used by GitHub Pages:

    bundle install

If it's been awhile since you installed the bundle, update it:

    bundle update

### Test the web site

Start a local web server by running:

    bundle exec jekyll serve --baseurl ''

Now you can access <http://127.0.0.1:4000> in your web browser to see the web site. When you make changes to your clone, your local site will automatically update. When done testing, press <kbd>Ctrl</kbd> + <kbd>C</kbd> to stop the local web server.

Changes to \_config.yml are not automatically reflected. If you change \_config.yml, stop and start the local web server to see those changes take effect.

---

These instructions are based on [GitHub's instructions](https://help.github.com/articles/setting-up-your-github-pages-site-locally-with-jekyll/).
