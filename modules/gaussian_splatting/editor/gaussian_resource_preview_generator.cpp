#ifdef TOOLS_ENABLED

#include "gaussian_resource_preview_generator.h"

#include "core/io/resource_loader.h"
#include "core/math/math_funcs.h"
#include "core/object/message_queue.h"
#include "core/object/object.h"
#include "core/os/semaphore.h"
#include "core/os/thread.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/texture.h"

#include "../core/gaussian_splat_asset.h"
#include "gaussian_thumbnail_generator.h"

void GaussianSplatAssetPreviewGenerator::_bind_methods() {}

class GaussianPreviewTextureCreateRequest : public Object {
public:
	Ref<Image> image;
	Ref<Texture2D> texture;
	Semaphore done;

	void create_texture() {
		if (image.is_valid() && !image->is_empty()) {
			texture = ImageTexture::create_from_image(image);
		}
		done.post();
	}
};

static Ref<Texture2D> _create_preview_texture_from_image(const Ref<Image> &p_image) {
	if (p_image.is_null() || p_image->is_empty()) {
		return Ref<Texture2D>();
	}
	if (Thread::is_main_thread()) {
		return ImageTexture::create_from_image(p_image);
	}

	CallQueue *main_queue = MessageQueue::get_main_singleton();
	ERR_FAIL_NULL_V_MSG(main_queue, Ref<Texture2D>(),
			"Gaussian preview texture creation must run on the main thread, but the main message queue is not available.");

	GaussianPreviewTextureCreateRequest request;
	request.image = p_image;
	const Error err = main_queue->push_callable(callable_mp(&request, &GaussianPreviewTextureCreateRequest::create_texture));
	ERR_FAIL_COND_V_MSG(err != OK, Ref<Texture2D>(), "Failed to queue Gaussian preview texture creation on the main thread.");

	request.done.wait();
	return request.texture;
}

bool GaussianSplatAssetPreviewGenerator::handles(const String &p_type) const {
	return ClassDB::is_parent_class(p_type, "GaussianSplatAsset");
}

Ref<Texture2D> GaussianSplatAssetPreviewGenerator::generate(const Ref<Resource> &p_from, const Size2 &p_size, Dictionary &p_metadata) const {
	Ref<GaussianSplatAsset> asset = p_from;
	if (asset.is_null()) {
		return Ref<Texture2D>();
	}

	if (Thread::is_main_thread()) {
		Ref<Texture2D> existing_thumbnail = asset->get_preview_texture();
		if (existing_thumbnail.is_valid()) {
			p_metadata[StringName("gaussian_preview_source")] = String("stored_thumbnail");
			return existing_thumbnail;
		}
	} else {
		Ref<Texture2D> existing_thumbnail = _create_preview_texture_from_image(asset->get_preview_image());
		if (existing_thumbnail.is_valid()) {
			p_metadata[StringName("gaussian_preview_source")] = String("stored_thumbnail");
			return existing_thumbnail;
		}
	}

	if (thumbnail_generator.is_null()) {
		thumbnail_generator.instantiate();
	}
	if (thumbnail_generator.is_null()) {
		return Ref<Texture2D>();
	}

	const int thumbnail_size = MAX(64, int(MAX(p_size.x, p_size.y)));
	p_metadata[StringName("gaussian_preview_source")] = String("generated_thumbnail");
	if (Thread::is_main_thread()) {
		return thumbnail_generator->generate_thumbnail(asset, thumbnail_size, GaussianThumbnailGenerator::THUMBNAIL_STYLE_COLOR);
	}
	return _create_preview_texture_from_image(
			thumbnail_generator->generate_thumbnail_image(asset, thumbnail_size, GaussianThumbnailGenerator::THUMBNAIL_STYLE_COLOR));
}

Ref<Texture2D> GaussianSplatAssetPreviewGenerator::generate_from_path(const String &p_path, const Size2 &p_size, Dictionary &p_metadata) const {
	Ref<GaussianSplatAsset> asset = ResourceLoader::load(p_path, "GaussianSplatAsset");
	if (asset.is_null()) {
		return Ref<Texture2D>();
	}
	p_metadata[StringName("source_path")] = p_path;
	return generate(asset, p_size, p_metadata);
}

bool GaussianSplatAssetPreviewGenerator::generate_small_preview_automatically() const {
	return true;
}

#endif // TOOLS_ENABLED
