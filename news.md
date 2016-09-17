---
title: News
redirect_from: /post/
---

{% for post in site.posts %}
## [{{ post.title }}]({{ post.url | prepend: site.baseurl }}) <small>{{ post.date | date: "%B %-d, %Y" }}</small>

{{ post.excerpt }}
{% if post.excerpt != post.content %}<a class="btn btn-default btn-xs" href="{{ post.url | prepend: site.baseurl }}">More <span class="glyphicon glyphicon-chevron-right" aria-hidden="true"></span></a>{% endif %}
{% endfor %}
