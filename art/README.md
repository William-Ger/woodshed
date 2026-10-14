Rendered UI art for Woodshed.

Regenerate with:

    /Applications/Blender.app/Contents/MacOS/Blender -b -P art/build_assets.py -- all

then convert renders/floor.png to renders/floor.jpg (quality ~86) and delete the PNG.
Each asset writes a JSON file with the normalised rects the app uses to line controls up with the pixels.
