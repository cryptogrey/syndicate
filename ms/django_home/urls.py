from django.conf.urls import patterns, include, url


urlpatterns = patterns('django_home.views',
                       url(r'^/?$', 'home'),
                       url(r'^allvolumes/?$', 'allvolumes'),
                       url(r'^myvolumes/?$', 'myvolumes'),
                       url(r'^settings/?$', 'settings'),
                       url(r'^downloads/?$','downloads'),
)